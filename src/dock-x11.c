/*
 * crepido
 *
 * X11 application discovery and activation for Dock launchers.
 *
 * The tracker deliberately uses normal X11/EWMH application windows rather
 * than the legacy DockApp protocol.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "dock-x11.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <gdk/gdkx.h>
#include <gio/gdesktopappinfo.h>

#include <errno.h>
#include <unistd.h>

#include <string.h>
#include <math.h>

#include "dock-launcher.h"
#include "dock-icon.h"
#include "dock-x11-match.h"
#include "dock-x11-property32.h"

/*
 * X11 client lists are inherently racy: a client can disappear between
 * reading _NET_CLIENT_LIST and querying one of that client's properties.
 * Treat BadWindow as a transient lookup miss instead of letting the
 * asynchronous X error escape through GDK.
 */
static XErrorHandler dock_x11_previous_error_handler = NULL;
static volatile gint dock_x11_bad_window_seen = 0;
static gboolean dock_x11_error_handler_installed = FALSE;

static int
dock_x11_error_handler(
    Display *display,
    XErrorEvent *event)
{
    if (event &&
        event->error_code == BadWindow) {
        dock_x11_bad_window_seen = 1;
        return 0;
    }

    if (dock_x11_previous_error_handler)
        return dock_x11_previous_error_handler(
            display,
            event);

    return 0;
}

static void
dock_x11_install_error_handler(void)
{
    if (dock_x11_error_handler_installed)
        return;

    dock_x11_bad_window_seen = 0;
    dock_x11_previous_error_handler =
        XSetErrorHandler(
            dock_x11_error_handler);

    dock_x11_error_handler_installed = TRUE;
}

static void
dock_x11_uninstall_error_handler(
    Display *display)
{
    if (!dock_x11_error_handler_installed)
        return;

    XSync(
        display,
        False);

    XSetErrorHandler(
        dock_x11_previous_error_handler);

    dock_x11_previous_error_handler = NULL;
    dock_x11_bad_window_seen = 0;
    dock_x11_error_handler_installed = FALSE;
}

struct _DockX11 {
    GtkWidget *container;
    guint refresh_id;

    GtkWidget *group_popup;
    GtkWidget *group_anchor;
    guint group_window_count;
    guint group_total_window_count;
    Window group_pending_activation;
    gboolean group_had_active_member;
    gboolean group_popup_suppressed;
    gboolean window_indicator_enabled;
    gboolean shortcut_only;
    gboolean minimized_window_icons;
    gboolean minimized_title_labels;
    gboolean minimized_group_drawer;
    gboolean minimized_all_workspaces;
    GtkWidget *miniwindow_popup;
    GtkWidget *mini_group_popup;
    GtkWidget *mini_group_anchor;
    GArray *mini_group_windows;
    Window minimized_scroll_window;
    gint64 minimized_scroll_time_us;
    GArray *minimized_scroll_windows;
    DockLauncher *minimized_scroll_launcher;
    guint mini_group_tick_id;
    guint mini_group_outside_poll_id;
    gint64 mini_group_animation_start_us;
    GtkWidget *miniwindow_box;
    gchar *miniwindow_signature;
    gint icon_size;
    gint opacity_percent;
    gint group_anchor_x;
    gint group_anchor_y;
    gint64 group_animation_start_us;
    gdouble group_animation_progress;
    gboolean group_animation_closing;
    guint group_tick_id;
    guint group_outside_poll_id;
    Display *display;
    Window root;

    Atom atom_client_list;
    Atom atom_active_window;
    Atom atom_current_desktop;
    Atom atom_number_of_desktops;
    Atom atom_net_wm_desktop;
    Atom atom_net_wm_pid;
    Atom atom_net_wm_name;
    Atom atom_utf8_string;
    Atom atom_net_wm_state;
    Atom atom_wm_protocols;
    Atom atom_delete_window;
    Atom atom_hidden;
    Atom atom_net_wm_window_type;
    Atom atom_dock_window_type;
    Atom atom_desktop_window_type;
};

static gint
dock_x11_tile_size(
    const DockX11 *x11)
{
    return (x11->icon_size * 64 + 24) / 48;
}


typedef struct {
    Window window;
    gint score;
    gboolean active;
    gboolean hidden;
    gboolean other_workspace;
    guint32 desktop;
} DockX11Match;

/*
 * One refresh-wide view of each X11 client. Window class and process
 * identity are comparatively expensive to query, so cache them once and
 * reuse them while matching each launcher.
 */
typedef struct {
    Window window;
    gchar *wm_class_name;
    gchar *wm_class_class;
    gchar *process_basename;
    gboolean dock_window;
    gboolean desktop_window;
} DockX11WindowInfo;

typedef struct {
    GArray *windows; /* DockX11WindowInfo */
    Window active;
    guint32 current_desktop;
} DockX11WindowSnapshot;

typedef struct {
    DockLauncher *launcher;
    GArray *windows;
} DockX11MinimizedGroup;

static void
dock_x11_minimized_group_free(gpointer data)
{
    DockX11MinimizedGroup *group = data;

    if (!group)
        return;

    g_clear_pointer(
        &group->windows,
        g_array_unref);

    g_free(group);
}

/*
 * These helpers are implemented later in this translation unit. The
 * shortcut-only miniwindow support is defined early, so keep the forward
 * declarations before that code.
 */
static gboolean atom_list_contains(
    Display *display,
    Window window,
    Atom property,
    Atom wanted);

static void activate_window(
    DockX11 *x11,
    Window window,
    guint32 timestamp);

static void dock_x11_request_close_window(
    DockX11 *x11,
    Window window,
    guint32 timestamp);

static guint32 get_window_desktop(
    DockX11 *x11,
    Window window);

static guint32 get_current_desktop(
    DockX11 *x11);

static Window get_active_window(
    DockX11 *x11);

static gchar *get_window_title(
    DockX11 *x11,
    Window window);

static gboolean get_client_windows(
    DockX11 *x11,
    Window **windows_out,
    gsize *count_out);

static gint score_window_for_app(
    DockX11 *x11,
    Window window,
    GAppInfo *app_info);

static GArray *find_matches(
    DockX11 *x11,
    GAppInfo *app_info);

static void dock_x11_activate_menu_window(
    GtkMenuItem *menu_item,
    gpointer user_data);

static GtkWidget *dock_x11_menu_item_new(
    const gchar *label,
    const gchar *icon_name);

static void dock_x11_append_window_actions_menu(
    DockX11 *x11,
    Window window,
    GtkMenuShell *menu_shell);

static gboolean dock_x11_minimized_button_press(
    GtkWidget *widget,
    GdkEventButton *event,
    gpointer user_data);

static gboolean dock_x11_minimized_scroll_event(
    GtkWidget *widget,
    GdkEventScroll *event,
    gpointer user_data);

static void dock_x11_minimized_scroll_state_reset(
    DockX11 *x11);

static void dock_x11_minimized_group_child_clicked(
    GtkButton *button,
    gpointer user_data);

static DockLauncher *dock_x11_find_window_launcher(
    DockX11 *x11,
    Window window);

static void dock_x11_minimized_group_popup_destroy(
    DockX11 *x11);


static void dock_x11_minimized_group_popup_refresh_states(
    DockX11 *x11);

static void
dock_x11_minimized_popup_destroy(DockX11 *x11)
{
    if (!x11)
        return;

    dock_x11_minimized_group_popup_destroy(x11);

    if (x11->miniwindow_popup) {
        gtk_widget_destroy(x11->miniwindow_popup);
        x11->miniwindow_popup = NULL;
        x11->miniwindow_box = NULL;
    }

    g_clear_pointer(
        &x11->miniwindow_signature,
        g_free);
}

static void
dock_x11_minimized_group_popup_destroy(
    DockX11 *x11)
{
    if (!x11)
        return;

    if (x11->mini_group_tick_id &&
        x11->mini_group_popup) {
        gtk_widget_remove_tick_callback(
            x11->mini_group_popup,
            x11->mini_group_tick_id);
    }

    x11->mini_group_tick_id = 0;

    if (x11->mini_group_outside_poll_id) {
        g_source_remove(
            x11->mini_group_outside_poll_id);
        x11->mini_group_outside_poll_id = 0;
    }

    if (x11->mini_group_popup) {
        gtk_widget_destroy(
            x11->mini_group_popup);
        x11->mini_group_popup = NULL;
    }

    x11->mini_group_anchor = NULL;

    g_clear_pointer(
        &x11->mini_group_windows,
        g_array_unref);

    x11->mini_group_animation_start_us = 0;
}

static void
dock_x11_minimized_group_popup_refresh_states(
    DockX11 *x11)
{
    if (!x11 ||
        !x11->mini_group_popup)
        return;

    Window active_window =
        get_active_window(x11);

    GtkWidget *child =
        gtk_bin_get_child(
            GTK_BIN(x11->mini_group_popup));

    if (!child ||
        !GTK_IS_FIXED(child))
        return;

    GList *cells =
        gtk_container_get_children(
            GTK_CONTAINER(child));

    for (GList *iter = cells;
         iter;
         iter = iter->next) {
        GtkWidget *cell =
            GTK_WIDGET(iter->data);

        GtkWidget *button =
            g_object_get_data(
                G_OBJECT(cell),
                "dock-mini-group-button");

        Window window =
            (Window)(guintptr)
            g_object_get_data(
                G_OBJECT(cell),
                "dock-miniwindow");

        if (!button ||
            !GTK_IS_WIDGET(button) ||
            window == None)
            continue;

        if (window == active_window) {
            gtk_widget_set_state_flags(
                button,
                GTK_STATE_FLAG_ACTIVE,
                TRUE);
        } else {
            gtk_widget_unset_state_flags(
                button,
                GTK_STATE_FLAG_ACTIVE);
        }
    }

    g_list_free(cells);
}

static gboolean
dock_x11_minimized_group_popup_point_inside(
    DockX11 *x11,
    gint root_x,
    gint root_y)
{
    if (!x11 ||
        !x11->mini_group_popup)
        return FALSE;

    GdkWindow *popup_window =
        gtk_widget_get_window(
            x11->mini_group_popup);

    if (popup_window) {
        gint popup_x = 0;
        gint popup_y = 0;

        gdk_window_get_origin(
            popup_window,
            &popup_x,
            &popup_y);

        GtkAllocation popup_allocation;
        gtk_widget_get_allocation(
            x11->mini_group_popup,
            &popup_allocation);

        if (root_x >= popup_x &&
            root_x < popup_x + popup_allocation.width &&
            root_y >= popup_y &&
            root_y < popup_y + popup_allocation.height)
            return TRUE;
    }

    GtkWidget *anchor = x11->mini_group_anchor;

    if (!anchor)
        return FALSE;

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(anchor);

    if (!toplevel ||
        !GTK_IS_WINDOW(toplevel))
        return FALSE;

    GdkWindow *anchor_window =
        gtk_widget_get_window(toplevel);

    if (!anchor_window)
        return FALSE;

    gint anchor_x = 0;
    gint anchor_y = 0;

    gdk_window_get_origin(
        anchor_window,
        &anchor_x,
        &anchor_y);

    GtkAllocation allocation;
    gtk_widget_get_allocation(
        anchor,
        &allocation);

    anchor_x += allocation.x;
    anchor_y += allocation.y;

    return root_x >= anchor_x &&
           root_x < anchor_x + allocation.width &&
           root_y >= anchor_y &&
           root_y < anchor_y + allocation.height;
}

static gboolean
dock_x11_minimized_group_popup_outside_poll(
    gpointer user_data)
{
    DockX11 *x11 = user_data;

    if (!x11 ||
        !x11->mini_group_popup) {
        return G_SOURCE_REMOVE;
    }

    Window root_return = None;
    Window child_return = None;
    gint root_x = 0;
    gint root_y = 0;
    gint win_x = 0;
    gint win_y = 0;
    unsigned int mask = 0;

    if (!XQueryPointer(
            x11->display,
            x11->root,
            &root_return,
            &child_return,
            &root_x,
            &root_y,
            &win_x,
            &win_y,
            &mask))
        return G_SOURCE_CONTINUE;

    if (!dock_x11_minimized_group_popup_point_inside(
            x11,
            root_x,
            root_y) &&
        !(mask &
          (Button1Mask |
           Button2Mask |
           Button3Mask))) {
        dock_x11_minimized_group_popup_destroy(x11);
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static gboolean
dock_x11_minimized_group_popup_tick(
    GtkWidget *widget,
    GdkFrameClock *frame_clock,
    gpointer user_data)
{
    DockX11 *x11 = user_data;

    if (!x11 ||
        !x11->mini_group_popup ||
        !x11->mini_group_windows)
        return G_SOURCE_REMOVE;

    gint64 now_us =
        gdk_frame_clock_get_frame_time(
            frame_clock);

    if (x11->mini_group_animation_start_us == 0)
        x11->mini_group_animation_start_us = now_us;

    gdouble progress =
        (gdouble)(
            now_us -
            x11->mini_group_animation_start_us) /
        160000.0;

    progress =
        CLAMP(
            progress,
            0.0,
            1.0);

    gdouble eased =
        1.0 -
        (1.0 - progress) *
        (1.0 - progress) *
        (1.0 - progress);

    GList *children =
        gtk_container_get_children(
            GTK_CONTAINER(
                gtk_bin_get_child(
                    GTK_BIN(widget))));

    for (GList *iter = children;
         iter;
         iter = iter->next) {
        GtkWidget *button =
            GTK_WIDGET(iter->data);

        guint index =
            GPOINTER_TO_UINT(
                g_object_get_data(
                    G_OBJECT(button),
                    "dock-mini-group-index"));

        GtkAllocation popup_allocation;
        gtk_widget_get_allocation(
            widget,
            &popup_allocation);

        gint tile_size =
            dock_x11_tile_size(x11);
        gint cell_height =
            tile_size +
            (x11->minimized_title_labels ? 18 : 0);

        gint final_y =
            ((gint)x11->mini_group_windows->len -
             1 -
             (gint)index) *
            cell_height;

        gint start_y =
            popup_allocation.height -
            cell_height;

        gint y =
            (gint)round(
                start_y +
                (final_y - start_y) *
                eased);

        gtk_fixed_move(
            GTK_FIXED(
                gtk_bin_get_child(
                    GTK_BIN(widget))),
            button,
            0,
            y);
    }

    g_list_free(children);

    if (progress >= 1.0) {
        x11->mini_group_tick_id = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static GtkWidget *
dock_x11_create_minimized_title_frame(
    const gchar *title,
    gint width)
{
    const gchar *text =
        title && *title ?
        title :
        "Untitled window";

    gint fixed_width =
        MAX(8, width);

    GtkWidget *frame =
        gtk_frame_new(NULL);

    gtk_frame_set_shadow_type(
        GTK_FRAME(frame),
        GTK_SHADOW_IN);

    gtk_widget_set_halign(
        frame,
        GTK_ALIGN_START);
    gtk_widget_set_valign(
        frame,
        GTK_ALIGN_START);
    gtk_widget_set_hexpand(
        frame,
        FALSE);
    gtk_widget_set_vexpand(
        frame,
        FALSE);
    gtk_widget_set_size_request(
        frame,
        fixed_width,
        18);

    GtkWidget *label =
        gtk_label_new(text);

    gtk_label_set_single_line_mode(
        GTK_LABEL(label),
        TRUE);
    gtk_label_set_ellipsize(
        GTK_LABEL(label),
        PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(
        GTK_LABEL(label),
        0.5);
    gtk_label_set_max_width_chars(
        GTK_LABEL(label),
        8);

    gint content_width =
        MAX(
            8,
            fixed_width - 6);

    gtk_widget_set_halign(
        label,
        GTK_ALIGN_FILL);
    gtk_widget_set_valign(
        label,
        GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(
        label,
        FALSE);
    gtk_widget_set_vexpand(
        label,
        FALSE);
    gtk_widget_set_size_request(
        label,
        content_width,
        18);

    gtk_widget_set_margin_start(
        label,
        3);
    gtk_widget_set_margin_end(
        label,
        3);

    /*
     * Use one small, consistent native GTK/Pango font for every group.
     * The size never changes from one application to another.
     */
    PangoAttrList *attrs =
        pango_attr_list_new();

    pango_attr_list_insert(
        attrs,
        pango_attr_size_new(
            8 * PANGO_SCALE));

    gtk_label_set_attributes(
        GTK_LABEL(label),
        attrs);

    gtk_container_add(
        GTK_CONTAINER(frame),
        label);

    gtk_widget_show_all(frame);

    return frame;
}

static gchar *
dock_x11_minimized_window_label(
    DockX11 *x11,
    Window window,
    gboolean include_workspace)
{
    gchar *title =
        get_window_title(
            x11,
            window);

    if (include_workspace) {
        guint32 desktop =
            get_window_desktop(
                x11,
                window);

        if (desktop != G_MAXUINT32 &&
            desktop != 0xFFFFFFFFU) {
            gchar *label =
                g_strdup_printf(
                    "%s — Workspace %u",
                    title && *title ?
                    title :
                    "Untitled window",
                    desktop + 1);

            g_free(title);
            return label;
        }
    }

    if (title && *title)
        return title;

    g_free(title);
    return g_strdup("Untitled window");
}

static void
dock_x11_show_minimized_group_drawer(
    DockX11 *x11,
    GtkWidget *anchor,
    GArray *group_windows)
{
    if (!x11 ||
        !anchor ||
        !group_windows ||
        group_windows->len < 2)
        return;

    dock_x11_minimized_group_popup_destroy(x11);

    gint tile_size =
        dock_x11_tile_size(x11);

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(anchor);

    if (!toplevel ||
        !GTK_IS_WINDOW(toplevel))
        return;

    GdkWindow *toplevel_window =
        gtk_widget_get_window(toplevel);

    if (!toplevel_window)
        return;

    gint window_x = 0;
    gint window_y = 0;

    gdk_window_get_origin(
        toplevel_window,
        &window_x,
        &window_y);

    GtkAllocation allocation;
    gtk_widget_get_allocation(
        anchor,
        &allocation);

    gint anchor_x =
        window_x +
        allocation.x;
    gint anchor_y =
        window_y +
        allocation.y;

    GtkWidget *popup =
        gtk_window_new(
            GTK_WINDOW_POPUP);

    gtk_window_set_decorated(
        GTK_WINDOW(popup),
        FALSE);
    gtk_window_set_resizable(
        GTK_WINDOW(popup),
        FALSE);
    gtk_window_set_skip_taskbar_hint(
        GTK_WINDOW(popup),
        TRUE);
    gtk_window_set_skip_pager_hint(
        GTK_WINDOW(popup),
        TRUE);
    gtk_window_set_type_hint(
        GTK_WINDOW(popup),
        GDK_WINDOW_TYPE_HINT_DOCK);
    gtk_window_set_accept_focus(
        GTK_WINDOW(popup),
        FALSE);
    gtk_window_set_focus_on_map(
        GTK_WINDOW(popup),
        FALSE);
    gtk_window_set_keep_above(
        GTK_WINDOW(popup),
        TRUE);
    gtk_widget_set_opacity(
        popup,
        x11->opacity_percent / 100.0);

    GtkWidget *fixed =
        gtk_fixed_new();

    gtk_container_add(
        GTK_CONTAINER(popup),
        fixed);

    for (guint i = 0;
         i < group_windows->len;
         i++) {
        Window window =
            g_array_index(
                group_windows,
                Window,
                i);

        DockLauncher *launcher =
            dock_x11_find_window_launcher(
                x11,
                window);

        if (!launcher) {
            gtk_widget_destroy(popup);
            return;
        }

        GtkWidget *button =
            dock_icon_button_new(
                launcher);

        if (!button) {
            gtk_widget_destroy(popup);
            return;
        }

        dock_icon_button_set_icon_size(
            button,
            x11->icon_size);

        gtk_widget_set_size_request(
            button,
            tile_size,
            tile_size);

        const gchar *app_name =
            dock_launcher_get_name(
                launcher);

        gchar *label =
            g_strdup(
                app_name && *app_name ?
                app_name :
                "Application");

        gchar *tooltip =
            dock_x11_minimized_window_label(
                x11,
                window,
                x11->minimized_all_workspaces);

        gchar *interaction_tooltip =
            g_strdup_printf(
                "%s\nLeft-click: restore\n"
                "Middle-click: close",
                tooltip);

        gtk_widget_set_tooltip_text(
            button,
            interaction_tooltip);

        g_free(interaction_tooltip);
        g_free(tooltip);

        GtkWidget *cell =
            gtk_overlay_new();

        gtk_widget_set_halign(
            cell,
            GTK_ALIGN_START);
        gtk_widget_set_valign(
            cell,
            GTK_ALIGN_START);
        gtk_widget_set_hexpand(
            cell,
            FALSE);
        gtk_widget_set_vexpand(
            cell,
            FALSE);
        gint cell_height =
            tile_size +
            (x11->minimized_title_labels ? 18 : 0);

        gtk_widget_set_size_request(
            cell,
            tile_size,
            cell_height);

        GtkWidget *base =
            gtk_fixed_new();

        gtk_widget_set_size_request(
            base,
            tile_size,
            cell_height);

        gtk_container_add(
            GTK_CONTAINER(cell),
            base);

        if (x11->minimized_title_labels) {
            GtkWidget *title_frame =
                dock_x11_create_minimized_title_frame(
                    label,
                    tile_size);

            gtk_overlay_add_overlay(
                GTK_OVERLAY(cell),
                title_frame);
        }

        gtk_fixed_put(
            GTK_FIXED(base),
            button,
            0,
            x11->minimized_title_labels ? 18 : 0);

        g_free(label);

        g_object_set_data(
            G_OBJECT(cell),
            "dock-miniwindow",
            (gpointer)(guintptr)window);

        g_object_set_data(
            G_OBJECT(cell),
            "dock-x11",
            x11);

        g_object_set_data(
            G_OBJECT(cell),
            "dock-mini-group-index",
            GUINT_TO_POINTER(i));

        g_object_set_data(
            G_OBJECT(cell),
            "dock-mini-group-button",
            button);

        g_object_set_data(
            G_OBJECT(button),
            "dock-miniwindow",
            (gpointer)(guintptr)window);

        g_object_set_data(
            G_OBJECT(button),
            "dock-x11",
            x11);

        g_signal_connect(
            button,
            "clicked",
            G_CALLBACK(
                dock_x11_minimized_group_child_clicked),
            x11);

        gtk_widget_add_events(
            button,
            GDK_SCROLL_MASK);

        g_signal_connect(
            button,
            "button-press-event",
            G_CALLBACK(
                dock_x11_minimized_button_press),
            x11);

        g_signal_connect(
            button,
            "scroll-event",
            G_CALLBACK(
                dock_x11_minimized_scroll_event),
            x11);

        gtk_fixed_put(
            GTK_FIXED(fixed),
            cell,
            0,
            0);
    }

    x11->mini_group_popup = popup;
    x11->mini_group_anchor = anchor;
    x11->mini_group_windows =
        g_array_ref(group_windows);
    x11->mini_group_animation_start_us = 0;

    dock_x11_minimized_group_popup_refresh_states(x11);

    gint cell_height =
        tile_size +
        (x11->minimized_title_labels ? 18 : 0);

    gint popup_height =
        (gint)group_windows->len * cell_height;

    gtk_window_set_default_size(
        GTK_WINDOW(popup),
        tile_size,
        popup_height);

    /*
     * The minimized strip lives along the bottom edge, so the expanded
     * group opens upward from the anchor instead of sideways. Keep the
     * popup on the same physical monitor and clamp it when the group is
     * close to a monitor edge.
     */
    gint popup_x =
        anchor_x;
    gint popup_y =
        anchor_y -
        popup_height;

    GdkDisplay *display =
        gtk_widget_get_display(anchor);

    if (display) {
        GdkMonitor *monitor =
            gdk_display_get_monitor_at_point(
                display,
                anchor_x + tile_size / 2,
                anchor_y + tile_size / 2);

        if (!monitor)
            monitor =
                gdk_display_get_primary_monitor(
                    display);

        if (monitor) {
            GdkRectangle geometry;
            gdk_monitor_get_geometry(
                monitor,
                &geometry);

            if (tile_size <= geometry.width) {
                gint min_x =
                    geometry.x;
                gint max_x =
                    geometry.x +
                    geometry.width -
                    tile_size;

                popup_x =
                    CLAMP(
                        popup_x,
                        min_x,
                        max_x);
            } else {
                popup_x =
                    geometry.x;
            }

            if (popup_height <= geometry.height) {
                gint min_y =
                    geometry.y;
                gint max_y =
                    geometry.y +
                    geometry.height -
                    popup_height;

                popup_y =
                    CLAMP(
                        popup_y,
                        min_y,
                        max_y);
            } else {
                popup_y =
                    geometry.y;
            }
        }
    }

    gtk_window_move(
        GTK_WINDOW(popup),
        popup_x,
        popup_y);

    gtk_widget_show_all(popup);

    x11->mini_group_tick_id =
        gtk_widget_add_tick_callback(
            popup,
            dock_x11_minimized_group_popup_tick,
            x11,
            NULL);

    x11->mini_group_outside_poll_id =
        g_timeout_add(
            25,
            dock_x11_minimized_group_popup_outside_poll,
            x11);
}

static void
dock_x11_show_minimized_group_menu(
    DockX11 *x11,
    GtkWidget *anchor,
    GArray *group_windows,
    GdkEventButton *event)
{
    if (!x11 ||
        !anchor ||
        !group_windows ||
        group_windows->len < 2)
        return;

    (void)anchor;

    GtkWidget *menu =
        gtk_menu_new();

    gtk_menu_set_reserve_toggle_size(
        GTK_MENU(menu),
        FALSE);

    for (guint i = 0;
         i < group_windows->len;
         i++) {
        Window window =
            g_array_index(
                group_windows,
                Window,
                i);

        gchar *label =
            dock_x11_minimized_window_label(
                x11,
                window,
                x11->minimized_all_workspaces);

        GtkWidget *item =
            dock_x11_menu_item_new(
                label,
                "window-new");

        g_object_set_data(
            G_OBJECT(item),
            "dock-x11",
            x11);

        g_object_set_data(
            G_OBJECT(item),
            "dock-group-window",
            (gpointer)(guintptr)window);

        g_signal_connect(
            item,
            "activate",
            G_CALLBACK(
                dock_x11_activate_menu_window),
            NULL);

        gtk_menu_shell_append(
            GTK_MENU_SHELL(menu),
            item);

        g_free(label);
    }

    gtk_widget_show_all(menu);

    gtk_menu_popup_at_pointer(
        GTK_MENU(menu),
        event ? (GdkEvent *)event : NULL);
}

static void
dock_x11_minimized_group_child_clicked(
    GtkButton *button,
    gpointer user_data)
{
    DockX11 *x11 = user_data;

    if (!button || !x11)
        return;

    if (!g_object_get_data(
            G_OBJECT(button),
            "dock-mini-group-child"))
        return;

    Window window =
        (Window)(guintptr)g_object_get_data(
            G_OBJECT(button),
            "dock-miniwindow");

    if (window == None)
        return;

    dock_x11_minimized_scroll_state_reset(
        x11);

    activate_window(
        x11,
        window,
        gtk_get_current_event_time());

    dock_x11_minimized_group_popup_destroy(x11);
}

static void
dock_x11_minimized_scroll_state_reset(
    DockX11 *x11)
{
    if (!x11)
        return;

    x11->minimized_scroll_window = None;
    x11->minimized_scroll_time_us = 0;
    g_clear_pointer(
        &x11->minimized_scroll_windows,
        g_array_unref);
    x11->minimized_scroll_launcher = NULL;
}

static gboolean
dock_x11_minimized_scroll_event(
    GtkWidget *widget,
    GdkEventScroll *event,
    gpointer user_data)
{
    DockX11 *x11 = user_data;

    if (!widget ||
        !event ||
        !x11)
        return FALSE;

    gint direction = 0;

    if (event->direction == GDK_SCROLL_UP)
        direction = -1;
    else if (event->direction == GDK_SCROLL_DOWN)
        direction = 1;
    else if (event->direction == GDK_SCROLL_SMOOTH) {
        if (fabs(event->delta_y) < 0.01 ||
            fabs(event->delta_y) <
            fabs(event->delta_x))
            return FALSE;

        direction =
            event->delta_y < 0.0 ?
            -1 :
            1;
    } else {
        return FALSE;
    }

    DockLauncher *launcher =
        g_object_get_data(
            G_OBJECT(widget),
            "dock-launcher");

    GArray *widget_group =
        g_object_get_data(
            G_OBJECT(widget),
            "dock-miniwindow-group");

    if ((!widget_group ||
         widget_group->len < 2) &&
        g_object_get_data(
            G_OBJECT(widget),
            "dock-mini-group-child") &&
        x11->mini_group_windows &&
        x11->mini_group_windows->len >= 2) {
        widget_group = x11->mini_group_windows;
    }

    /*
     * The current minimized strip can be rebuilt immediately after restoring
     * a window. Keep our own copy of the original group so the next wheel
     * event can still reach that restored window without touching the old
     * button or re-running application matching.
     */
    if ((!x11->minimized_scroll_windows ||
         x11->minimized_scroll_windows->len < 2) ||
        launcher != x11->minimized_scroll_launcher) {
        if (!widget_group ||
            widget_group->len < 2)
            return FALSE;

        GArray *copy =
            g_array_sized_new(
                FALSE,
                FALSE,
                sizeof(Window),
                widget_group->len);

        g_array_append_vals(
            copy,
            widget_group->data,
            widget_group->len);

        g_clear_pointer(
            &x11->minimized_scroll_windows,
            g_array_unref);

        x11->minimized_scroll_windows = copy;
        x11->minimized_scroll_launcher = launcher;
    }

    if (!x11->minimized_scroll_windows ||
        x11->minimized_scroll_windows->len < 2)
        return FALSE;

    /*
     * Drop windows that have actually disappeared since the group was
     * captured. Restored windows remain in _NET_CLIENT_LIST, so they stay
     * available for cycling even though they are no longer minimized.
     */
    Window *client_windows = NULL;
    gsize client_count = 0;

    if (get_client_windows(
            x11,
            &client_windows,
            &client_count)) {
        for (gint i =
                 (gint)x11->minimized_scroll_windows->len - 1;
             i >= 0;
             i--) {
            Window candidate =
                g_array_index(
                    x11->minimized_scroll_windows,
                    Window,
                    (guint)i);

            gboolean present = FALSE;

            for (gsize j = 0;
                 j < client_count;
                 j++) {
                if (client_windows[j] == candidate) {
                    present = TRUE;
                    break;
                }
            }

            if (!present) {
                if (candidate ==
                    x11->minimized_scroll_window) {
                    x11->minimized_scroll_window = None;
                }

                g_array_remove_index(
                    x11->minimized_scroll_windows,
                    (guint)i);
            }
        }

        g_free(client_windows);
    }

    if (x11->minimized_scroll_windows->len < 2)
        return FALSE;

    guint64 now_us =
        (guint64)g_get_monotonic_time();

    gboolean recent =
        x11->minimized_scroll_window != None &&
        x11->minimized_scroll_time_us > 0 &&
        now_us >=
            (guint64)x11->minimized_scroll_time_us &&
        now_us -
            (guint64)x11->minimized_scroll_time_us <=
            750000;

    gint base_index = -1;

    if (recent) {
        for (guint i = 0;
             i < x11->minimized_scroll_windows->len;
             i++) {
            Window candidate =
                g_array_index(
                    x11->minimized_scroll_windows,
                    Window,
                    i);

            if (candidate ==
                x11->minimized_scroll_window) {
                base_index = (gint)i;
                break;
            }
        }
    }

    if (base_index < 0) {
        Window active_window =
            get_active_window(x11);

        for (guint i = 0;
             i < x11->minimized_scroll_windows->len;
             i++) {
            Window candidate =
                g_array_index(
                    x11->minimized_scroll_windows,
                    Window,
                    i);

            if (candidate == active_window) {
                base_index = (gint)i;
                break;
            }
        }
    }

    gint next_index;

    if (base_index < 0) {
        next_index =
            direction > 0 ?
            0 :
            (gint)x11->minimized_scroll_windows->len - 1;
    } else {
        next_index =
            base_index +
            (direction > 0 ? 1 : -1);

        if (next_index < 0)
            next_index =
                (gint)x11->minimized_scroll_windows->len - 1;
        else if (next_index >=
                 (gint)x11->minimized_scroll_windows->len)
            next_index = 0;
    }

    Window next =
        g_array_index(
            x11->minimized_scroll_windows,
            Window,
            (guint)next_index);

    if (next == None)
        return FALSE;

    if (x11->mini_group_popup)
        dock_x11_minimized_group_popup_destroy(x11);

    x11->minimized_scroll_window = next;
    x11->minimized_scroll_time_us =
        (gint64)now_us;

    activate_window(
        x11,
        next,
        event->time);

    return TRUE;
}

static gboolean
dock_x11_minimized_button_press(
    GtkWidget *widget,
    GdkEventButton *event,
    gpointer user_data)
{

    if (!widget ||
        !event)
        return FALSE;

    DockX11 *x11 = user_data;

    if (!x11)
        return FALSE;

    if (event->button == GDK_BUTTON_SECONDARY) {
        GArray *group_windows =
            g_object_get_data(
                G_OBJECT(widget),
                "dock-miniwindow-group");

        if (group_windows &&
            group_windows->len > 1) {
            /*
             * A grouped minimized icon represents several windows. Do not
             * silently pick the first one for the context menu; let the
             * user choose the specific window to restore.
             */
            dock_x11_show_minimized_group_menu(
                x11,
                widget,
                group_windows,
                event);
            return TRUE;
        }

        Window window =
            (Window)(guintptr)g_object_get_data(
                G_OBJECT(widget),
                "dock-miniwindow");

        if (window != None) {
            GtkWidget *menu =
                gtk_menu_new();

            gtk_menu_set_reserve_toggle_size(
                GTK_MENU(menu),
                FALSE);

            dock_x11_append_window_actions_menu(
                x11,
                window,
                GTK_MENU_SHELL(menu));

            gtk_widget_show_all(menu);

            gtk_menu_popup_at_pointer(
                GTK_MENU(menu),
                (GdkEvent *)event);
        }

        return TRUE;
    }
    GArray *group_windows =
        g_object_get_data(
            G_OBJECT(widget),
            "dock-miniwindow-group");

    if (group_windows &&
        group_windows->len > 0) {
        if (event->button == GDK_BUTTON_PRIMARY &&
            group_windows->len > 1 &&
            x11->minimized_window_icons &&
            x11->minimized_group_drawer &&
            x11->mini_group_popup &&
            x11->mini_group_anchor == widget) {
            dock_x11_minimized_group_popup_destroy(x11);
            return TRUE;
        }

        Window window =
            g_array_index(
                group_windows,
                Window,
                0);

        if (event->button == GDK_BUTTON_MIDDLE) {
            dock_x11_request_close_window(
                x11,
                window,
                event->time);
            return TRUE;
        }

        if (event->button == GDK_BUTTON_PRIMARY) {
            if (group_windows->len > 1) {
                if (x11->minimized_window_icons &&
                    x11->minimized_group_drawer) {
                    dock_x11_show_minimized_group_drawer(
                        x11,
                        widget,
                        group_windows);
                } else {
                    dock_x11_show_minimized_group_menu(
                        x11,
                        widget,
                        group_windows,
                        event);
                }

                return TRUE;
            }

            activate_window(
                x11,
                window,
                event->time);
            return TRUE;
        }

        return FALSE;
    }

    Window window =
        (Window)(guintptr)g_object_get_data(
            G_OBJECT(widget),
            "dock-miniwindow");

    if (window == None)
        return TRUE;

    if (event->button == GDK_BUTTON_MIDDLE) {
        dock_x11_request_close_window(
            x11,
            window,
            event->time);

        if (g_object_get_data(
                G_OBJECT(widget),
                "dock-mini-group-child"))
            dock_x11_minimized_group_popup_destroy(x11);

        return TRUE;
    }

    if (event->button == GDK_BUTTON_PRIMARY) {
        if (g_object_get_data(
                G_OBJECT(widget),
                "dock-mini-group-child"))
            return FALSE;

        dock_x11_minimized_scroll_state_reset(x11);

        activate_window(
            x11,
            window,
            event->time);

        return TRUE;
    }

    return FALSE;
}

static DockLauncher *
dock_x11_find_window_launcher(
    DockX11 *x11,
    Window window)
{
    if (!x11 || !x11->container)
        return NULL;

    GList *children =
        gtk_container_get_children(
            GTK_CONTAINER(x11->container));

    DockLauncher *best_launcher = NULL;
    gint best_score = G_MININT;

    for (GList *iter = children;
         iter;
         iter = iter->next) {
        GtkWidget *button =
            GTK_WIDGET(iter->data);

        DockLauncher *launcher =
            g_object_get_data(
                G_OBJECT(button),
                "dock-launcher");

        if (!launcher)
            continue;

        gint score =
            score_window_for_app(
                x11,
                window,
                dock_launcher_get_app_info(
                    launcher));

        if (score >= 100 &&
            score > best_score) {
            best_launcher = launcher;
            best_score = score;
        }
    }

    g_list_free(children);
    return best_launcher;
}

static GIcon *
dock_x11_find_window_icon(
    DockX11 *x11,
    Window window)
{
    DockLauncher *launcher =
        dock_x11_find_window_launcher(
            x11,
            window);

    if (!launcher)
        return NULL;

    return g_app_info_get_icon(
        dock_launcher_get_app_info(
            launcher));
}

static gboolean
dock_x11_is_window_minimized_on_current_desktop(
    DockX11 *x11,
    Window window,
    guint32 current_desktop,
    gboolean all_workspaces)
{
    if (window == None)
        return FALSE;

    if (atom_list_contains(
            x11->display,
            window,
            x11->atom_net_wm_window_type,
            x11->atom_dock_window_type) ||
        atom_list_contains(
            x11->display,
            window,
            x11->atom_net_wm_window_type,
            x11->atom_desktop_window_type))
        return FALSE;

    if (!atom_list_contains(
            x11->display,
            window,
            x11->atom_net_wm_state,
            x11->atom_hidden))
        return FALSE;

    guint32 desktop =
        get_window_desktop(
            x11,
            window);

    if (!all_workspaces &&
        desktop != G_MAXUINT32 &&
        desktop != 0xFFFFFFFFU &&
        current_desktop != G_MAXUINT32 &&
        current_desktop != 0xFFFFFFFFU &&
        desktop != current_desktop)
        return FALSE;

    return TRUE;
}

static void
dock_x11_minimized_popup_position(
    DockX11 *x11,
    guint count)
{
    if (!x11 ||
        !x11->miniwindow_popup)
        return;

    GdkDisplay *display =
        gtk_widget_get_display(
            x11->container);

    if (!display)
        return;

    GdkMonitor *monitor = NULL;
    GdkWindow *dock_window =
        gtk_widget_get_window(
            x11->container);

    if (dock_window)
        monitor =
            gdk_display_get_monitor_at_window(
                display,
                dock_window);

    if (!monitor)
        monitor =
            gdk_display_get_primary_monitor(
                display);

    if (!monitor)
        return;

    GdkRectangle geometry;
    gdk_monitor_get_geometry(
        monitor,
        &geometry);

    gint square_tile_size =
        dock_x11_tile_size(x11);
    gint tile_width =
        x11->minimized_window_icons ?
        square_tile_size :
        160;
    gint tile_height =
        x11->minimized_window_icons ?
        square_tile_size +
        (x11->minimized_title_labels ? 18 : 0) :
        48;
    gint width =
        MIN(
            geometry.width,
            MAX(tile_width, (gint)count * tile_width));
    gint height = tile_height;

    gtk_window_resize(
        GTK_WINDOW(x11->miniwindow_popup),
        width,
        height);

    gtk_window_move(
        GTK_WINDOW(x11->miniwindow_popup),
        geometry.x,
        geometry.y +
            geometry.height -
            height);
}

static void
dock_x11_minimized_refresh(
    DockX11 *x11)
{
    if (!x11)
        return;

    if (!x11->shortcut_only) {
        dock_x11_minimized_popup_destroy(x11);
        return;
    }

    Window *windows = NULL;
    gsize window_count = 0;

    if (!get_client_windows(
            x11,
            &windows,
            &window_count)) {
        dock_x11_minimized_popup_destroy(x11);
        return;
    }

    guint32 current_desktop =
        get_current_desktop(x11);

    GString *signature =
        g_string_new(NULL);

    GArray *minimized =
        g_array_new(
            FALSE,
            FALSE,
            sizeof(Window));

    for (gsize i = 0;
         i < window_count;
         i++) {
        Window window = windows[i];

        if (!dock_x11_is_window_minimized_on_current_desktop(
                x11,
                window,
                current_desktop,
                x11->minimized_all_workspaces))
            continue;

        gchar *title =
            get_window_title(
                x11,
                window);

        g_string_append_printf(
            signature,
            "%lu:",
            (unsigned long)window);

        if (title) {
            g_string_append(
                signature,
                title);
        }

        g_string_append_c(
            signature,
            ';');

        g_free(title);

        g_array_append_val(
            minimized,
            window);
    }

    g_free(windows);

    if (minimized->len == 0) {
        g_array_unref(minimized);
        g_string_free(signature, TRUE);
        dock_x11_minimized_popup_destroy(x11);
        return;
    }

    guint display_count = minimized->len;

    if (g_strcmp0(
            x11->miniwindow_signature,
            signature->str) != 0 ||
        !x11->miniwindow_popup) {
        dock_x11_minimized_popup_destroy(x11);

        /*
         * Minimized windows are represented by a normal, non-focusable
         * managed window. A GTK popup would stay above maximized clients,
         * which unnecessarily covers the user's workspace.
         */
        x11->miniwindow_popup =
            gtk_window_new(
                GTK_WINDOW_TOPLEVEL);

        gtk_window_set_decorated(
            GTK_WINDOW(x11->miniwindow_popup),
            FALSE);
        gtk_window_set_resizable(
            GTK_WINDOW(x11->miniwindow_popup),
            FALSE);
        gtk_window_set_skip_taskbar_hint(
            GTK_WINDOW(x11->miniwindow_popup),
            TRUE);
        gtk_window_set_skip_pager_hint(
            GTK_WINDOW(x11->miniwindow_popup),
            TRUE);
        gtk_window_set_type_hint(
            GTK_WINDOW(x11->miniwindow_popup),
            GDK_WINDOW_TYPE_HINT_NORMAL);
        gtk_window_set_accept_focus(
            GTK_WINDOW(x11->miniwindow_popup),
            FALSE);
        gtk_window_set_focus_on_map(
            GTK_WINDOW(x11->miniwindow_popup),
            FALSE);
        gtk_widget_set_opacity(
            x11->miniwindow_popup,
            x11->opacity_percent / 100.0);

        x11->miniwindow_box =
            gtk_box_new(
                GTK_ORIENTATION_HORIZONTAL,
                x11->minimized_window_icons ? 0 : 4);

        gtk_container_set_border_width(
            GTK_CONTAINER(x11->miniwindow_box),
            x11->minimized_window_icons ? 0 : 2);

        gtk_container_add(
            GTK_CONTAINER(x11->miniwindow_popup),
            x11->miniwindow_box);

        GPtrArray *groups =
            g_ptr_array_new_with_free_func(
                dock_x11_minimized_group_free);

        for (guint i = 0;
             i < minimized->len;
             i++) {
            Window window =
                g_array_index(
                    minimized,
                    Window,
                    i);

            DockLauncher *launcher =
                dock_x11_find_window_launcher(
                    x11,
                    window);

            DockX11MinimizedGroup *group = NULL;

            if (launcher) {
                for (guint j = 0;
                     j < groups->len;
                     j++) {
                    DockX11MinimizedGroup *candidate =
                        g_ptr_array_index(
                            groups,
                            j);

                    if (candidate->launcher == launcher) {
                        group = candidate;
                        break;
                    }
                }
            }

            if (!group) {
                group =
                    g_new0(
                        DockX11MinimizedGroup,
                        1);

                group->launcher = launcher;
                group->windows =
                    g_array_new(
                        FALSE,
                        FALSE,
                        sizeof(Window));

                g_ptr_array_add(
                    groups,
                    group);
            }

            g_array_append_val(
                group->windows,
                window);
        }

        for (guint i = 0;
             i < groups->len;
             i++) {
            DockX11MinimizedGroup *group =
                g_ptr_array_index(
                    groups,
                    i);

            guint group_count =
                group->windows->len;

            Window window =
                g_array_index(
                    group->windows,
                    Window,
                    0);

            gchar *title =
                get_window_title(
                    x11,
                    window);

            GtkWidget *button = NULL;

            if (x11->minimized_window_icons &&
                group->launcher) {
                button =
                    dock_icon_button_new(
                        group->launcher);

                gint square_tile_size =
                    dock_x11_tile_size(x11);

                gtk_widget_set_size_request(
                    button,
                    square_tile_size,
                    square_tile_size);

                dock_icon_button_set_icon_size(
                    button,
                    x11->icon_size);

                dock_icon_button_set_instance_count(
                    button,
                    group_count);

                gchar *tooltip =
                    x11->minimized_all_workspaces ?
                    g_strdup_printf(
                        "%s (%u minimized across workspaces)",
                        dock_launcher_get_name(
                            group->launcher),
                        group_count) :
                    g_strdup_printf(
                        "%s (%u minimized)",
                        dock_launcher_get_name(
                            group->launcher),
                        group_count);

                gchar *interaction_tooltip =
                    g_strdup_printf(
                        "%s\nLeft-click: show windows\n"
                        "Wheel: cycle windows\n"
                        "Middle-click: close a window",
                        tooltip);

                gtk_widget_set_tooltip_text(
                    button,
                    interaction_tooltip);

                g_free(interaction_tooltip);
                g_free(tooltip);

                g_object_set_data_full(
                    G_OBJECT(button),
                    "dock-miniwindow-group",
                    g_array_ref(group->windows),
                    (GDestroyNotify)g_array_unref);
            } else {
                GIcon *icon =
                    dock_x11_find_window_icon(
                        x11,
                        window);

                if (group->launcher) {
                    GtkWidget *box =
                        gtk_box_new(
                            GTK_ORIENTATION_HORIZONTAL,
                            6);

                    button =
                        gtk_button_new();
                    gtk_style_context_add_class(
                        gtk_widget_get_style_context(button),
                        "crepido-bitmap-block");

                    gtk_button_set_relief(
                        GTK_BUTTON(button),
                        GTK_RELIEF_NORMAL);
                    gtk_widget_set_can_focus(
                        button,
                        FALSE);
                    gtk_widget_set_focus_on_click(
                        button,
                        FALSE);
                    gtk_widget_set_size_request(
                        button,
                        154,
                        44);

                    GtkWidget *image =
                        icon ?
                        gtk_image_new_from_gicon(
                            icon,
                            GTK_ICON_SIZE_BUTTON) :
                        gtk_image_new_from_icon_name(
                            "application-x-executable",
                            GTK_ICON_SIZE_BUTTON);

                    gtk_image_set_pixel_size(
                        GTK_IMAGE(image),
                        MIN(
                            32,
                            MAX(
                                16,
                                x11->icon_size)));

                    const gchar *app_name =
                        dock_launcher_get_name(
                            group->launcher);

                    gchar *label_text = NULL;

                    if (group_count > 1) {
                        label_text =
                            g_strdup_printf(
                                "%s (%u)",
                                app_name && *app_name ?
                                app_name :
                                (title && *title ?
                                 title :
                                 "Untitled window"),
                                group_count);
                    } else {
                        label_text =
                            g_strdup(
                                title && *title ?
                                title :
                                (app_name && *app_name ?
                                 app_name :
                                 "Untitled window"));
                    }

                    GtkWidget *label =
                        gtk_label_new(
                            label_text);

                    gtk_label_set_xalign(
                        GTK_LABEL(label),
                        0.0);
                    gtk_label_set_ellipsize(
                        GTK_LABEL(label),
                        PANGO_ELLIPSIZE_END);
                    gtk_label_set_max_width_chars(
                        GTK_LABEL(label),
                        28);

                    gtk_box_pack_start(
                        GTK_BOX(box),
                        image,
                        FALSE,
                        FALSE,
                        0);
                    gtk_box_pack_start(
                        GTK_BOX(box),
                        label,
                        TRUE,
                        TRUE,
                        0);

                    gtk_container_add(
                        GTK_CONTAINER(button),
                        box);

                    gchar *tooltip =
                        group_count > 1 ?
                        (x11->minimized_all_workspaces ?
                         g_strdup_printf(
                            "%s (%u minimized across workspaces)",
                            app_name && *app_name ?
                            app_name :
                            "Application",
                            group_count) :
                         g_strdup_printf(
                            "%s (%u minimized)",
                            app_name && *app_name ?
                            app_name :
                            "Application",
                            group_count)) :
                        dock_x11_minimized_window_label(
                            x11,
                            window,
                            x11->minimized_all_workspaces);

                    gchar *interaction_tooltip =
                        g_strdup_printf(
                            "%s\nLeft-click: show windows\n"
                            "Wheel: cycle windows\n"
                            "Middle-click: close a window",
                            tooltip);

                    gtk_widget_set_tooltip_text(
                        button,
                        interaction_tooltip);

                    g_free(interaction_tooltip);
                    g_free(tooltip);
                    g_free(label_text);

                    g_object_set_data_full(
                        G_OBJECT(button),
                        "dock-miniwindow-group",
                        g_array_ref(group->windows),
                        (GDestroyNotify)g_array_unref);
                } else {
                    button =
                        gtk_button_new();
                    gtk_style_context_add_class(
                        gtk_widget_get_style_context(button),
                        "crepido-bitmap-block");

                    gtk_button_set_relief(
                        GTK_BUTTON(button),
                        GTK_RELIEF_NORMAL);
                    gtk_widget_set_can_focus(
                        button,
                        FALSE);
                    gtk_widget_set_focus_on_click(
                        button,
                        FALSE);
                    gtk_widget_set_size_request(
                        button,
                        154,
                        44);

                    GtkWidget *label =
                        gtk_label_new(
                            title && *title ?
                            title :
                            "Untitled window");

                    gtk_label_set_xalign(
                        GTK_LABEL(label),
                        0.0);
                    gtk_label_set_ellipsize(
                        GTK_LABEL(label),
                        PANGO_ELLIPSIZE_END);
                    gtk_label_set_max_width_chars(
                        GTK_LABEL(label),
                        28);

                    gtk_container_add(
                        GTK_CONTAINER(button),
                        label);

                    g_object_set_data(
                        G_OBJECT(button),
                        "dock-miniwindow",
                        (gpointer)(guintptr)window);
                }
            }

            gtk_widget_add_events(
                button,
                GDK_SCROLL_MASK);

            g_signal_connect(
                button,
                "button-press-event",
                G_CALLBACK(
                    dock_x11_minimized_button_press),
                x11);

            g_signal_connect(
                button,
                "scroll-event",
                G_CALLBACK(
                    dock_x11_minimized_scroll_event),
                x11);

            if (x11->minimized_window_icons &&
                group->launcher) {
                GtkWidget *cell =
                    gtk_overlay_new();

                gtk_widget_set_halign(
                    cell,
                    GTK_ALIGN_START);
                gtk_widget_set_valign(
                    cell,
                    GTK_ALIGN_START);
                gtk_widget_set_hexpand(
                    cell,
                    FALSE);
                gtk_widget_set_vexpand(
                    cell,
                    FALSE);
                gint cell_height =
                    dock_x11_tile_size(x11) +
                    (x11->minimized_title_labels ? 18 : 0);

                gtk_widget_set_size_request(
                    cell,
                    dock_x11_tile_size(x11),
                    cell_height);

                GtkWidget *base =
                    gtk_fixed_new();

                gtk_widget_set_size_request(
                    base,
                    dock_x11_tile_size(x11),
                    cell_height);

                gtk_container_add(
                    GTK_CONTAINER(cell),
                    base);

                if (x11->minimized_title_labels) {
                    const gchar *app_name =
                        dock_launcher_get_name(
                            group->launcher);

                    gchar *title_label =
                        g_strdup(
                            app_name && *app_name ?
                            app_name :
                            "Application");

                    GtkWidget *title_frame =
                        dock_x11_create_minimized_title_frame(
                            title_label,
                            dock_x11_tile_size(x11));

                    gtk_overlay_add_overlay(
                        GTK_OVERLAY(cell),
                        title_frame);

                    g_free(title_label);
                }

                gtk_fixed_put(
                    GTK_FIXED(base),
                    button,
                    0,
                    x11->minimized_title_labels ? 18 : 0);

                gtk_box_pack_start(
                    GTK_BOX(x11->miniwindow_box),
                    cell,
                    FALSE,
                    FALSE,
                    0);
            } else {
                gtk_box_pack_start(
                    GTK_BOX(x11->miniwindow_box),
                    button,
                    FALSE,
                    FALSE,
                    0);
            }

            g_free(title);
        }

        display_count =
            x11->minimized_window_icons ?
            groups->len :
            minimized->len;

        g_ptr_array_free(
            groups,
            TRUE);

        g_free(x11->miniwindow_signature);
        x11->miniwindow_signature =
            g_strdup(signature->str);

        gtk_widget_show_all(
            x11->miniwindow_popup);
    }

    dock_x11_minimized_popup_position(
        x11,
        display_count);

    g_array_unref(minimized);
    g_string_free(signature, TRUE);
}

static gint score_window_for_app(
    DockX11 *x11,
    Window window,
    GAppInfo *app_info);

static gchar *get_window_title(
    DockX11 *x11,
    Window window);

static gboolean get_client_windows(
    DockX11 *x11,
    Window **windows_out,
    gsize *count_out);

static gboolean dock_x11_refresh(gpointer user_data);

static GtkWidget *
dock_x11_menu_item_new(
    const gchar *label,
    const gchar *icon_name)
{
    GtkWidget *item =
        gtk_menu_item_new();

    GtkWidget *box =
        gtk_box_new(
            GTK_ORIENTATION_HORIZONTAL,
            6);

    GtkWidget *icon_box =
        gtk_box_new(
            GTK_ORIENTATION_HORIZONTAL,
            0);

    GtkWidget *image =
        gtk_image_new_from_icon_name(
            icon_name,
            GTK_ICON_SIZE_MENU);

    GtkWidget *text =
        gtk_label_new(label);

    gtk_widget_set_size_request(
        icon_box,
        16,
        -1);

    gtk_widget_set_halign(
        text,
        GTK_ALIGN_START);

    gtk_widget_set_hexpand(
        text,
        TRUE);

    gtk_box_pack_start(
        GTK_BOX(icon_box),
        image,
        FALSE,
        FALSE,
        0);

    gtk_box_pack_start(
        GTK_BOX(box),
        icon_box,
        FALSE,
        FALSE,
        0);

    gtk_box_pack_start(
        GTK_BOX(box),
        text,
        TRUE,
        TRUE,
        0);

    gtk_container_add(
        GTK_CONTAINER(item),
        box);

    return item;
}

static gboolean
atom_list_contains(
    Display *display,
    Window window,
    Atom property,
    Atom wanted);

static void
dock_x11_activate_menu_window(
    GtkMenuItem *menu_item,
    gpointer user_data);

static void
activate_window(
    DockX11 *x11,
    Window window,
    guint32 timestamp);

static void
minimize_window(
    DockX11 *x11,
    Window window);

static guint32
get_number_of_desktops(
    DockX11 *x11);

static void
move_window_to_desktop(
    DockX11 *x11,
    Window window,
    guint32 desktop);

static void
dock_x11_group_window_state_toggle(
    GtkMenuItem *menu_item,
    gpointer user_data);

static void
dock_x11_group_window_move_desktop(
    GtkMenuItem *menu_item,
    gpointer user_data);

static void
dock_x11_append_window_actions_menu(
    DockX11 *x11,
    Window window,
    GtkMenuShell *menu_shell);

static void
dock_x11_group_popup_close(DockX11 *x11);

static void
dock_x11_group_popup_destroy(
    DockX11 *x11);

static void
dock_x11_activate_menu_window(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    (void)user_data;

    DockX11 *x11 =
        g_object_get_data(
            G_OBJECT(menu_item),
            "dock-x11");

    Window window =
        (Window)(guintptr)g_object_get_data(
            G_OBJECT(menu_item),
            "dock-group-window");

    if (!x11 || window == None)
        return;

    activate_window(
        x11,
        window,
        gtk_get_current_event_time());
}

static guint32
get_window_desktop(
    DockX11 *x11,
    Window window);

static guint32
get_current_desktop(
    DockX11 *x11);

static void
dock_x11_group_outside_poll_stop(
    DockX11 *x11);

static void
dock_x11_request_close_window(
    DockX11 *x11,
    Window window,
    guint32 timestamp)
{
    if (!x11 || window == None)
        return;

    XEvent event;
    memset(&event, 0, sizeof(event));

    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type =
        x11->atom_wm_protocols;
    event.xclient.format = 32;
    event.xclient.data.l[0] =
        x11->atom_delete_window;
    event.xclient.data.l[1] =
        timestamp != 0 ?
        timestamp :
        CurrentTime;

    XSendEvent(
        x11->display,
        window,
        False,
        NoEventMask,
        &event);

    XFlush(x11->display);
}

static void
dock_x11_close_group_window(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    (void)user_data;

    DockX11 *x11 =
        g_object_get_data(
            G_OBJECT(menu_item),
            "dock-x11");

    Window window =
        (Window)(guintptr)g_object_get_data(
            G_OBJECT(menu_item),
            "dock-group-window");

    dock_x11_request_close_window(
        x11,
        window,
        gtk_get_current_event_time());
}

static gboolean
dock_x11_group_popup_tick(
    GtkWidget *widget,
    GdkFrameClock *frame_clock,
    gpointer user_data);

static void
dock_x11_group_window_clicked(
    GtkButton *button,
    gpointer user_data)
{
    DockX11 *x11 = user_data;

    Window window =
        (Window)(guintptr)g_object_get_data(
            G_OBJECT(button),
            "dock-group-window");

    if (window == None)
        return;

    x11->group_pending_activation =
        window;

    activate_window(
        x11,
        window,
        gtk_get_current_event_time());
}

static gboolean
dock_x11_group_window_button_press(
    GtkWidget *widget,
    GdkEventButton *event,
    gpointer user_data)
{
    (void)widget;

    DockX11 *x11 = user_data;

    if (event->button != GDK_BUTTON_SECONDARY &&
        event->button != GDK_BUTTON_MIDDLE)
        return FALSE;

    Window window =
        (Window)(guintptr)g_object_get_data(
            G_OBJECT(widget),
            "dock-group-window");

    if (window == None)
        return TRUE;

    if (event->button == GDK_BUTTON_MIDDLE) {
        dock_x11_request_close_window(
            x11,
            window,
            event->time);
        return TRUE;
    }

    GtkWidget *menu =
        gtk_menu_new();

    gtk_menu_set_reserve_toggle_size(
        GTK_MENU(menu),
        FALSE);

    dock_x11_append_window_actions_menu(
        x11,
        window,
        GTK_MENU_SHELL(menu));

    gtk_widget_show_all(menu);

    gtk_menu_popup_at_pointer(
        GTK_MENU(menu),
        (GdkEvent *)event);

    return TRUE;
}

static gboolean
atom_list_contains(
    Display *display,
    Window window,
    Atom property,
    Atom wanted)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        display,
        window,
        property,
        0,
        32,
        False,
        XA_ATOM,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &data);

    if (status != Success ||
        actual_type != XA_ATOM ||
        actual_format != 32 ||
        !data) {
        if (data)
            XFree(data);
        return FALSE;
    }

    Atom *atoms = (Atom *)data;
    gboolean found = FALSE;

    for (unsigned long i = 0; i < item_count; i++) {
        if (atoms[i] == wanted) {
            found = TRUE;
            break;
        }
    }

    XFree(data);
    return found;
}

static gboolean
get_window_pid(
    Display *display,
    Window window,
    Atom pid_atom,
    pid_t *pid_out)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        display,
        window,
        pid_atom,
        0,
        1,
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

    guint32 raw_pid = 0;

    gboolean read_ok =
        dock_x11_property32_read(
            (const unsigned long *)data,
            item_count,
            0,
            &raw_pid);

    XFree(data);

    if (!read_ok || raw_pid == 0)
        return FALSE;

    *pid_out = (pid_t)raw_pid;
    return TRUE;
}

static gchar *
get_process_executable_basename(pid_t pid)
{
    gchar *proc_link =
        g_strdup_printf("/proc/%ld/exe", (long)pid);

    gchar *target =
        g_file_read_link(proc_link, NULL);

    g_free(proc_link);

    if (!target)
        return NULL;

    gchar *basename =
        g_path_get_basename(target);

    g_free(target);
    return basename;
}

static gboolean
same_name_ci(
    const gchar *a,
    const gchar *b)
{
    if (!a || !b || !*a || !*b)
        return FALSE;

    return g_ascii_strcasecmp(a, b) == 0;
}

static gboolean
wm_class_matches(
    Display *display,
    Window window,
    const gchar *wanted)
{
    if (!wanted || !*wanted)
        return FALSE;

    XClassHint hint;
    memset(&hint, 0, sizeof(hint));

    if (!XGetClassHint(display, window, &hint))
        return FALSE;

    gboolean matched =
        same_name_ci(hint.res_name, wanted) ||
        same_name_ci(hint.res_class, wanted);

    if (hint.res_name)
        XFree(hint.res_name);

    if (hint.res_class)
        XFree(hint.res_class);

    return matched;
}

static gchar *
desktop_exec_basename(
    GAppInfo *app_info)
{
    const gchar *command =
        g_app_info_get_commandline(app_info);

    if (!command || !*command)
        return NULL;

    gint argc = 0;
    gchar **argv = NULL;

    if (!g_shell_parse_argv(
            command,
            &argc,
            &argv,
            NULL) ||
        argc <= 0) {
        g_strfreev(argv);
        return NULL;
    }

    /*
     * Desktop Exec lines can prefix the real command with "env".
     * Skip the common environment-assignment/options form enough to find
     * the executable without trying to implement a shell.
     */
    gint i = 0;

    if (g_strcmp0(argv[i], "env") == 0) {
        i++;

        while (i < argc) {
            if (g_strcmp0(argv[i], "--") == 0) {
                i++;
                break;
            }

            if (g_str_has_prefix(argv[i], "-") ||
                strchr(argv[i], '=') != NULL) {
                i++;
                continue;
            }

            break;
        }
    }

    gchar *basename =
        (i < argc) ?
        g_path_get_basename(argv[i]) :
        NULL;

    g_strfreev(argv);
    return basename;
}

static gint
score_window_for_app(
    DockX11 *x11,
    Window window,
    GAppInfo *app_info)
{
    if (!G_IS_DESKTOP_APP_INFO(app_info))
        return 0;

    gchar *startup_wm_class =
        g_strdup(
            g_desktop_app_info_get_startup_wm_class(
                G_DESKTOP_APP_INFO(app_info)));

    gchar *exec_basename =
        desktop_exec_basename(app_info);

    gint score = 0;

    if (startup_wm_class &&
        wm_class_matches(
            x11->display,
            window,
            startup_wm_class)) {
        score = 200;
        goto done;
    }

    if (exec_basename &&
        wm_class_matches(
            x11->display,
            window,
            exec_basename)) {
        score = 100;
    }

    pid_t pid = 0;

    if (exec_basename &&
        get_window_pid(
            x11->display,
            window,
            x11->atom_net_wm_pid,
            &pid)) {
        gchar *process_basename =
            get_process_executable_basename(pid);

        if (process_basename &&
            same_name_ci(
                process_basename,
                exec_basename)) {
            score = MAX(score, 220);
        }

        g_free(process_basename);
    }

done:
    g_free(startup_wm_class);
    g_free(exec_basename);

    return score;
}

static gchar *
get_window_title(
    DockX11 *x11,
    Window window)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        x11->display,
        window,
        x11->atom_net_wm_name,
        0,
        1024,
        False,
        x11->atom_utf8_string,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &data);

    if (status == Success &&
        actual_type == x11->atom_utf8_string &&
        actual_format == 8 &&
        data &&
        item_count > 0) {
        gchar *title =
            g_strndup(
                (const gchar *)data,
                item_count);

        XFree(data);
        return title;
    }

    if (data)
        XFree(data);

    gchar *legacy_title = NULL;

    if (XFetchName(
            x11->display,
            window,
            &legacy_title) &&
        legacy_title) {
        gchar *title =
            g_strdup(legacy_title);

        XFree(legacy_title);
        return title;
    }

    return NULL;
}

static gboolean
get_client_windows(
    DockX11 *x11,
    Window **windows_out,
    gsize *count_out)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        x11->display,
        x11->root,
        x11->atom_client_list,
        0,
        4096,
        False,
        XA_WINDOW,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &data);

    if (status != Success ||
        actual_type != XA_WINDOW ||
        actual_format != 32 ||
        !data) {
        if (data)
            XFree(data);
        return FALSE;
    }

    *windows_out = g_new(
        Window,
        item_count);

    memcpy(
        *windows_out,
        data,
        item_count * sizeof(Window));

    *count_out = item_count;

    XFree(data);
    return TRUE;
}

static Window
get_active_window(DockX11 *x11)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        x11->display,
        x11->root,
        x11->atom_active_window,
        0,
        1,
        False,
        XA_WINDOW,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &data);

    if (status != Success ||
        actual_type != XA_WINDOW ||
        actual_format != 32 ||
        item_count < 1 ||
        !data) {
        if (data)
            XFree(data);
        return None;
    }

    Window active = ((Window *)data)[0];
    XFree(data);
    return active;
}

static void
dock_x11_window_snapshot_clear(
    DockX11WindowSnapshot *snapshot)
{
    if (!snapshot)
        return;

    if (snapshot->windows) {
        for (guint i = 0; i < snapshot->windows->len; i++) {
            DockX11WindowInfo *info =
                &g_array_index(
                    snapshot->windows,
                    DockX11WindowInfo,
                    i);

            g_free(info->wm_class_name);
            g_free(info->wm_class_class);
            g_free(info->process_basename);
        }

        g_array_unref(snapshot->windows);
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->active = None;
    snapshot->current_desktop = G_MAXUINT32;
}

static gboolean
dock_x11_window_snapshot_load(
    DockX11 *x11,
    DockX11WindowSnapshot *snapshot)
{
    g_return_val_if_fail(snapshot != NULL, FALSE);

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->active = None;
    snapshot->current_desktop = G_MAXUINT32;

    Window *windows = NULL;
    gsize count = 0;

    if (!get_client_windows(
            x11,
            &windows,
            &count))
        return FALSE;

    snapshot->windows =
        g_array_new(
            FALSE,
            FALSE,
            sizeof(DockX11WindowInfo));

    snapshot->active = get_active_window(x11);
    snapshot->current_desktop = get_current_desktop(x11);

    for (gsize i = 0; i < count; i++) {
        Window window = windows[i];

        if (window == None)
            continue;

        DockX11WindowInfo info = {
            .window = window
        };

        /*
         * These window types cannot be application instances. Cache this
         * classification once instead of querying it for every launcher.
         */
        info.dock_window =
            atom_list_contains(
                x11->display,
                window,
                x11->atom_net_wm_window_type,
                x11->atom_dock_window_type);

        if (!info.dock_window) {
            info.desktop_window =
                atom_list_contains(
                    x11->display,
                    window,
                    x11->atom_net_wm_window_type,
                    x11->atom_desktop_window_type);
        }

        if (!info.dock_window && !info.desktop_window) {
            XClassHint hint;
            memset(&hint, 0, sizeof(hint));

            if (XGetClassHint(
                    x11->display,
                    window,
                    &hint)) {
                if (hint.res_name)
                    info.wm_class_name =
                        g_strdup(hint.res_name);

                if (hint.res_class)
                    info.wm_class_class =
                        g_strdup(hint.res_class);
            }

            if (hint.res_name)
                XFree(hint.res_name);

            if (hint.res_class)
                XFree(hint.res_class);

            pid_t pid = 0;

            if (get_window_pid(
                    x11->display,
                    window,
                    x11->atom_net_wm_pid,
                    &pid)) {
                info.process_basename =
                    get_process_executable_basename(pid);
            }
        }

        g_array_append_val(
            snapshot->windows,
            info);
    }

    g_free(windows);

    /*
     * A client can disappear while its properties are read. Reject an
     * incomplete snapshot rather than giving different launchers different
     * views of the same refresh.
     */
    XSync(
        x11->display,
        False);

    if (dock_x11_bad_window_seen) {
        dock_x11_bad_window_seen = 0;
        dock_x11_window_snapshot_clear(snapshot);
        return FALSE;
    }

    return TRUE;
}

static gint
score_window_info_for_app(
    const DockX11WindowInfo *info,
    const gchar *startup_wm_class,
    const gchar *exec_basename)
{
    const DockX11MatchCandidate candidate = {
        .wm_class_name = info->wm_class_name,
        .wm_class_class = info->wm_class_class,
        .process_basename = info->process_basename,
        .dock_window = info->dock_window,
        .desktop_window = info->desktop_window
    };

    return dock_x11_match_score(
        &candidate,
        startup_wm_class,
        exec_basename);
}

static GArray *
find_matches_from_snapshot(
    DockX11 *x11,
    GAppInfo *app_info,
    const DockX11WindowSnapshot *snapshot)
{
    GArray *matches =
        g_array_new(
            FALSE,
            FALSE,
            sizeof(DockX11Match));

    if (!snapshot ||
        !snapshot->windows ||
        !G_IS_DESKTOP_APP_INFO(app_info))
        return matches;

    const gchar *startup_wm_class =
        g_desktop_app_info_get_startup_wm_class(
            G_DESKTOP_APP_INFO(app_info));

    gchar *exec_basename =
        desktop_exec_basename(app_info);

    for (guint i = 0; i < snapshot->windows->len; i++) {
        DockX11WindowInfo *info =
            &g_array_index(
                snapshot->windows,
                DockX11WindowInfo,
                i);

        /*
         * The shared scoring helper excludes dock/desktop windows and only
         * accepts strong application identity.
         */
        gint score =
            score_window_info_for_app(
                info,
                startup_wm_class,
                exec_basename);

        if (score < DOCK_X11_MATCH_MIN_SCORE)
            continue;

        guint32 desktop =
            get_window_desktop(
                x11,
                info->window);

        gboolean other_workspace =
            desktop != G_MAXUINT32 &&
            desktop != 0xFFFFFFFFU &&
            snapshot->current_desktop != G_MAXUINT32 &&
            snapshot->current_desktop != 0xFFFFFFFFU &&
            desktop != snapshot->current_desktop;

        DockX11Match match = {
            .window = info->window,
            .score = score,
            .active = (info->window == snapshot->active),
            .hidden =
                atom_list_contains(
                    x11->display,
                    info->window,
                    x11->atom_net_wm_state,
                    x11->atom_hidden),
            .other_workspace = other_workspace,
            .desktop = desktop
        };

        g_array_append_val(
            matches,
            match);
    }

    g_free(exec_basename);

    /*
     * A matched window may also vanish after the shared identity snapshot
     * was built. Treat that launcher as having no matches for this pass.
     */
    XSync(
        x11->display,
        False);

    if (dock_x11_bad_window_seen) {
        g_array_set_size(
            matches,
            0);
        dock_x11_bad_window_seen = 0;
    }

    return matches;
}

static GArray *
find_matches(
    DockX11 *x11,
    GAppInfo *app_info)
{
    DockX11WindowSnapshot snapshot;
    dock_x11_window_snapshot_load(
        x11,
        &snapshot);

    GArray *matches =
        find_matches_from_snapshot(
            x11,
            app_info,
            &snapshot);

    dock_x11_window_snapshot_clear(&snapshot);

    return matches;
}

static void
dock_x11_set_group_button_tooltip(
    GtkWidget *button,
    const gchar *tooltip)
{
    const gchar *old_tooltip =
        g_object_get_data(
            G_OBJECT(button),
            "dock-group-tooltip");

    if (g_strcmp0(old_tooltip, tooltip) == 0)
        return;

    g_object_set_data_full(
        G_OBJECT(button),
        "dock-group-tooltip",
        g_strdup(tooltip ? tooltip : ""),
        g_free);

    gtk_widget_set_tooltip_text(
        button,
        tooltip);
}

static gboolean
dock_x11_group_active_window_belongs(
    GArray *matches,
    Window active_window)
{
    if (active_window == None)
        return FALSE;

    for (guint i = 0; i < matches->len; i++) {
        DockX11Match *match =
            &g_array_index(
                matches,
                DockX11Match,
                i);

        if (match->window == active_window)
            return TRUE;
    }

    return FALSE;
}

static void
dock_x11_group_popup_refresh_states(
    DockX11 *x11,
    GArray *matches)
{
    if (!x11->group_popup ||
        !x11->group_anchor ||
        !matches)
        return;

    GtkWidget *fixed =
        g_object_get_data(
            G_OBJECT(x11->group_popup),
            "dock-group-fixed");

    if (!fixed || !GTK_IS_FIXED(fixed))
        return;

    GList *children =
        gtk_container_get_children(
            GTK_CONTAINER(fixed));

    for (GList *iter = children; iter; iter = iter->next) {
        GtkWidget *button =
            GTK_WIDGET(iter->data);

        Window window =
            (Window)(guintptr)g_object_get_data(
                G_OBJECT(button),
                "dock-group-window");

        gboolean hidden = FALSE;
        gboolean active = FALSE;
        gboolean other_workspace = FALSE;
        guint32 desktop = G_MAXUINT32;

        for (guint i = 0; i < matches->len; i++) {
            DockX11Match *match =
                &g_array_index(
                    matches,
                    DockX11Match,
                    i);

            if (match->window == window) {
                hidden = match->hidden;
                active = match->active;
                other_workspace = match->other_workspace;
                desktop = match->desktop;
                break;
            }
        }

        /*
         * Keep the tooltip synchronized with the live X11 window title.
         * Applications such as Caja and terminals can change titles while
         * the expansion remains open.
         */
        DockLauncher *launcher =
            g_object_get_data(
                G_OBJECT(x11->group_anchor),
                "dock-launcher");

        gchar *window_title =
            get_window_title(
                x11,
                window);

        gchar *tooltip = NULL;

        if (window_title && *window_title) {
            if (desktop != G_MAXUINT32 &&
                desktop != 0xFFFFFFFFU) {
                tooltip =
                    g_strdup_printf(
                        "%s — Workspace %u",
                        window_title,
                        desktop + 1);
            } else {
                tooltip =
                    g_strdup(window_title);
            }
        } else if (launcher) {
            tooltip =
                g_strdup(
                    dock_launcher_get_name(
                        launcher));
        }

        if (tooltip) {
            dock_x11_set_group_button_tooltip(
                button,
                tooltip);
            g_free(tooltip);
        }

        g_free(window_title);

        /*
         * Hidden/minimized windows remain fully clickable so they can be
         * restored, but are visually subdued to distinguish them from
         * visible windows.
         */
        gdouble opacity =
            hidden ? 0.55 :
            other_workspace ? 0.75 :
            1.0;

        gtk_widget_set_opacity(
            button,
            opacity);

        if (active) {
            gtk_widget_set_state_flags(
                button,
                GTK_STATE_FLAG_ACTIVE,
                TRUE);
        } else {
            gtk_widget_unset_state_flags(
                button,
                GTK_STATE_FLAG_ACTIVE);
        }
    }

    g_list_free(children);
}

static void
dock_x11_group_popup_position(
    DockX11 *x11,
    gdouble progress)
{
    if (!x11->group_popup)
        return;

    gint popup_width =
        (gint)x11->group_window_count *
        dock_x11_tile_size(x11);

    if (popup_width <= 0)
        return;

    /*
     * Keep the popup at its final geometry for the whole animation. Only
     * the individual blocks move inside it. This avoids X11/GTK resizing
     * jitter while preserving the fixed right edge against the Dock icon.
     */
    gtk_window_move(
        GTK_WINDOW(x11->group_popup),
        x11->group_anchor_x - popup_width,
        x11->group_anchor_y);

    GtkWidget *fixed =
        g_object_get_data(
            G_OBJECT(x11->group_popup),
            "dock-group-fixed");

    if (!fixed || !GTK_IS_FIXED(fixed))
        return;

    GList *children =
        gtk_container_get_children(
            GTK_CONTAINER(fixed));

    gint index = 0;

    for (GList *iter = children; iter; iter = iter->next) {
        GtkWidget *button =
            GTK_WIDGET(iter->data);

        /*
         * At progress 0 all blocks overlap at the Dock-facing right edge.
         * At progress 1 they occupy their final adjacent positions.
         */
        gint final_x =
            popup_width -
            dock_x11_tile_size(x11) * (index + 1);

        gint start_x =
            popup_width -
            dock_x11_tile_size(x11);

        gint x =
            (gint)round(
                start_x +
                (final_x - start_x) * progress);

        gtk_fixed_move(
            GTK_FIXED(fixed),
            button,
            x,
            0);

        index++;
    }

    g_list_free(children);
}

static gboolean
dock_x11_group_popup_tick(
    GtkWidget *widget,
    GdkFrameClock *frame_clock,
    gpointer user_data)
{
    (void)widget;

    DockX11 *x11 = user_data;

    if (!x11->group_popup)
        return G_SOURCE_REMOVE;

    gint64 now_us =
        gdk_frame_clock_get_frame_time(frame_clock);

    gdouble elapsed =
        (gdouble)(now_us - x11->group_animation_start_us) /
        180000.0;

    elapsed = CLAMP(elapsed, 0.0, 1.0);

    gdouble linear_progress =
        x11->group_animation_closing ?
        1.0 - elapsed :
        elapsed;

    /*
     * Cubic ease-out when opening and ease-in when closing. The popup
     * geometry stays fixed, so this is the only visual motion involved.
     */
    gdouble progress;

    if (x11->group_animation_closing) {
        progress =
            linear_progress *
            linear_progress *
            linear_progress;
    } else {
        gdouble inverse =
            1.0 - linear_progress;

        progress =
            1.0 -
            inverse * inverse * inverse;
    }

    x11->group_animation_progress =
        CLAMP(progress, 0.0, 1.0);

    dock_x11_group_popup_position(
        x11,
        x11->group_animation_progress);

    if (elapsed >= 1.0) {
        x11->group_tick_id = 0;

        if (x11->group_animation_closing) {
            dock_x11_group_outside_poll_stop(
                x11);

            gtk_widget_destroy(
                x11->group_popup);

            x11->group_popup = NULL;
            x11->group_anchor = NULL;
            x11->group_window_count = 0;
            x11->group_total_window_count = 0;
        }

        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static void
dock_x11_group_outside_poll_stop(
    DockX11 *x11)
{
    if (!x11->group_outside_poll_id)
        return;

    g_source_remove(
        x11->group_outside_poll_id);

    x11->group_outside_poll_id = 0;
}

static void
dock_x11_group_popup_start_animation(
    DockX11 *x11,
    gboolean closing)
{
    if (!x11->group_popup)
        return;

    if (x11->group_tick_id) {
        gtk_widget_remove_tick_callback(
            x11->group_popup,
            x11->group_tick_id);
        x11->group_tick_id = 0;
    }

    x11->group_animation_closing = closing;
    x11->group_animation_start_us =
        g_get_monotonic_time();

    x11->group_animation_progress =
        closing ? 1.0 : 0.0;

    dock_x11_group_popup_position(
        x11,
        x11->group_animation_progress);

    x11->group_tick_id =
        gtk_widget_add_tick_callback(
            x11->group_popup,
            dock_x11_group_popup_tick,
            x11,
            NULL);
}


void
dock_x11_close_group_popup(
    DockX11 *x11)
{
    if (!x11)
        return;

    dock_x11_group_popup_destroy(x11);
}

gint
dock_x11_prepare_group_popup_for_press(
    DockX11 *x11,
    GtkWidget *button)
{
    if (!x11 ||
        !button ||
        !x11->group_popup)
        return 0;

    gboolean same_anchor =
        x11->group_anchor == button;

    dock_x11_group_popup_destroy(x11);

    return same_anchor ? 2 : 1;
}

void
dock_x11_set_group_popup_suppressed(
    DockX11 *x11,
    gboolean suppressed)
{
    if (!x11)
        return;

    x11->group_popup_suppressed = suppressed;

    if (suppressed)
        dock_x11_group_popup_destroy(x11);
}

void
dock_x11_close_group_for_ancestor(
    DockX11 *x11,
    GtkWidget *ancestor)
{
    if (!x11 ||
        !ancestor ||
        !x11->group_popup ||
        !x11->group_anchor)
        return;

    if (gtk_widget_is_ancestor(
            x11->group_anchor,
            ancestor)) {
        dock_x11_group_popup_destroy(x11);
    }
}

static gboolean
dock_x11_group_point_inside(
    DockX11 *x11,
    gint root_x,
    gint root_y)
{
    if (!x11->group_popup ||
        !x11->group_anchor)
        return FALSE;

    GdkWindow *popup_window =
        gtk_widget_get_window(
            x11->group_popup);

    if (popup_window) {
        gint popup_x = 0;
        gint popup_y = 0;

        gdk_window_get_origin(
            popup_window,
            &popup_x,
            &popup_y);

        GtkAllocation popup_allocation;
        gtk_widget_get_allocation(
            x11->group_popup,
            &popup_allocation);

        if (root_x >= popup_x &&
            root_x < popup_x + popup_allocation.width &&
            root_y >= popup_y &&
            root_y < popup_y + popup_allocation.height) {
            return TRUE;
        }
    }

    GdkWindow *anchor_toplevel =
        gtk_widget_get_window(
            gtk_widget_get_toplevel(
                x11->group_anchor));

    if (!anchor_toplevel)
        return FALSE;

    gint window_x = 0;
    gint window_y = 0;

    gdk_window_get_origin(
        anchor_toplevel,
        &window_x,
        &window_y);

    GtkAllocation anchor_allocation;
    gtk_widget_get_allocation(
        x11->group_anchor,
        &anchor_allocation);

    gint anchor_x =
        window_x + anchor_allocation.x;
    gint anchor_y =
        window_y + anchor_allocation.y;

    return root_x >= anchor_x &&
           root_x < anchor_x + anchor_allocation.width &&
           root_y >= anchor_y &&
           root_y < anchor_y + anchor_allocation.height;
}

gboolean
dock_x11_group_popup_point_inside(
    DockX11 *x11,
    GtkWidget *ancestor,
    gint root_x,
    gint root_y)
{
    if (!x11 ||
        !ancestor ||
        !x11->group_popup ||
        !x11->group_anchor)
        return FALSE;

    if (x11->group_anchor != ancestor &&
        !gtk_widget_is_ancestor(
            x11->group_anchor,
            ancestor))
        return FALSE;

    return dock_x11_group_point_inside(
        x11,
        root_x,
        root_y);
}

static gboolean
dock_x11_group_outside_poll(gpointer user_data)
{
    DockX11 *x11 = user_data;

    if (!x11->group_popup)
        return G_SOURCE_REMOVE;

    Window root_return = None;
    Window child_return = None;
    gint root_x = 0;
    gint root_y = 0;
    gint win_x = 0;
    gint win_y = 0;
    unsigned int mask = 0;

    if (!XQueryPointer(
            x11->display,
            x11->root,
            &root_return,
            &child_return,
            &root_x,
            &root_y,
            &win_x,
            &win_y,
            &mask)) {
        return G_SOURCE_CONTINUE;
    }

    if (!dock_x11_group_point_inside(
            x11,
            root_x,
            root_y) &&
        (mask &
         (Button1Mask |
          Button2Mask |
          Button3Mask))) {
        dock_x11_group_popup_close(x11);
    }

    return G_SOURCE_CONTINUE;
}

static void
dock_x11_group_popup_destroy(
    DockX11 *x11)
{
    if (!x11->group_popup)
        return;

    if (x11->group_tick_id) {
        gtk_widget_remove_tick_callback(
            x11->group_popup,
            x11->group_tick_id);
        x11->group_tick_id = 0;
    }

    dock_x11_group_outside_poll_stop(x11);

    gtk_widget_destroy(
        x11->group_popup);

    x11->group_popup = NULL;
    x11->group_anchor = NULL;
    x11->group_window_count = 0;
    x11->group_total_window_count = 0;
    x11->group_pending_activation = None;
    x11->group_had_active_member = FALSE;
}

static void
dock_x11_group_popup_close(DockX11 *x11)
{
    if (!x11->group_popup)
        return;

    dock_x11_group_popup_start_animation(
        x11,
        TRUE);
}

static gboolean
dock_x11_group_popup_open(
    DockX11 *x11,
    GtkWidget *anchor,
    DockLauncher *launcher,
    GArray *matches)
{
    if (matches->len < 2)
        return FALSE;

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(anchor);

    if (!toplevel ||
        !GTK_IS_WINDOW(toplevel))
        return FALSE;

    GdkWindow *toplevel_window =
        gtk_widget_get_window(toplevel);

    if (!toplevel_window)
        return FALSE;

    gint window_x = 0;
    gint window_y = 0;

    gdk_window_get_origin(
        toplevel_window,
        &window_x,
        &window_y);

    GtkAllocation allocation;
    gtk_widget_get_allocation(
        anchor,
        &allocation);

    /*
     * Dock buttons are child widgets without their own X11/GDK windows.
     * The top-level window origin therefore has to be combined with the
     * button's position inside the Dock's GtkFixed.
     */
    gint anchor_x =
        window_x + allocation.x;
    gint anchor_y =
        window_y + allocation.y;

    GtkWidget *popup =
        gtk_window_new(GTK_WINDOW_POPUP);

    gtk_window_set_decorated(
        GTK_WINDOW(popup),
        FALSE);
    gtk_window_set_resizable(
        GTK_WINDOW(popup),
        FALSE);
    gtk_window_set_skip_taskbar_hint(
        GTK_WINDOW(popup),
        TRUE);
    gtk_window_set_skip_pager_hint(
        GTK_WINDOW(popup),
        TRUE);
    gtk_window_set_type_hint(
        GTK_WINDOW(popup),
        GDK_WINDOW_TYPE_HINT_DOCK);
    gtk_window_set_accept_focus(
        GTK_WINDOW(popup),
        FALSE);
    gtk_window_set_keep_above(
        GTK_WINDOW(popup),
        TRUE);

    gtk_widget_set_opacity(
        popup,
        x11->opacity_percent / 100.0);

    gtk_window_set_screen(
        GTK_WINDOW(popup),
        gtk_widget_get_screen(anchor));

    GtkWidget *fixed =
        gtk_fixed_new();

    gtk_container_add(
        GTK_CONTAINER(popup),
        fixed);

    g_object_set_data(
        G_OBJECT(popup),
        "dock-group-fixed",
        fixed);

    gint active_index = -1;

    for (guint i = 0; i < matches->len; i++) {
        DockX11Match *match =
            &g_array_index(
                matches,
                DockX11Match,
                i);

        if (match->active) {
            active_index = (gint)i;
            break;
        }
    }

    /*
     * Keep the currently active window represented by the anchor icon.
     * Every other matching window becomes one of the sliding blocks.
     */
    guint extra_count = 0;

    for (guint i = 0; i < matches->len; i++) {
        if ((gint)i == active_index)
            continue;

        DockX11Match *match =
            &g_array_index(
                matches,
                DockX11Match,
                i);

        GtkWidget *button =
            dock_icon_button_new(launcher);

        dock_icon_button_set_icon_size(
            button,
            x11->icon_size);

        gtk_widget_set_size_request(
            button,
            dock_x11_tile_size(x11),
            dock_x11_tile_size(x11));

        g_object_set_data(
            G_OBJECT(button),
            "dock-group-window",
            (gpointer)(guintptr)match->window);

        gchar *window_title =
            get_window_title(
                x11,
                match->window);

        gchar *tooltip = NULL;

        if (window_title && *window_title) {
            if (match->desktop != G_MAXUINT32 &&
                match->desktop != 0xFFFFFFFFU) {
                tooltip =
                    g_strdup_printf(
                        "%s — Workspace %u",
                        window_title,
                        match->desktop + 1);
            } else {
                tooltip =
                    g_strdup(window_title);
            }
        } else {
            tooltip =
                g_strdup(
                    dock_launcher_get_name(launcher));
        }

        dock_x11_set_group_button_tooltip(
            button,
            tooltip);

        g_free(tooltip);
        g_free(window_title);

        g_signal_connect(
            button,
            "clicked",
            G_CALLBACK(dock_x11_group_window_clicked),
            x11);

        g_signal_connect(
            button,
            "button-press-event",
            G_CALLBACK(dock_x11_group_window_button_press),
            x11);

        gtk_widget_add_events(
            button,
            GDK_BUTTON_PRESS_MASK);

        gtk_fixed_put(
            GTK_FIXED(fixed),
            button,
            0,
            0);

        extra_count++;
    }

    if (extra_count == 0) {
        gtk_widget_destroy(popup);
        return FALSE;
    }

    x11->group_popup = popup;
    x11->group_anchor = anchor;
    x11->group_window_count = extra_count;
    x11->group_total_window_count = matches->len;
    x11->group_anchor_x = anchor_x;
    x11->group_anchor_y = anchor_y;
    x11->group_pending_activation = None;
    x11->group_had_active_member = FALSE;

    dock_x11_group_popup_refresh_states(
        x11,
        matches);

    gint popup_width =
        (gint)extra_count * dock_x11_tile_size(x11);

    gtk_window_set_default_size(
        GTK_WINDOW(popup),
        popup_width,
        dock_x11_tile_size(x11));

    gtk_window_move(
        GTK_WINDOW(popup),
        anchor_x - popup_width,
        anchor_y);

    gtk_widget_show_all(popup);

    x11->group_outside_poll_id =
        g_timeout_add(
            25,
            dock_x11_group_outside_poll,
            x11);

    /*
     * The popup keeps its final width from the first frame. The blocks
     * themselves start stacked against the Dock-facing edge and animate
     * into their final positions.
     */
    dock_x11_group_popup_position(
        x11,
        0.0);

    dock_x11_group_popup_start_animation(
        x11,
        FALSE);

    return TRUE;
}

static void
dock_x11_append_window_actions_menu(
    DockX11 *x11,
    Window window,
    GtkMenuShell *menu_shell)
{
    if (!x11 ||
        window == None ||
        !GTK_IS_MENU_SHELL(menu_shell))
        return;

    gchar *window_title =
        get_window_title(
            x11,
            window);

    if (!window_title || !*window_title) {
        g_free(window_title);
        window_title = g_strdup("Window");
    }

    GtkWidget *title_item =
        gtk_menu_item_new_with_label(
            window_title);

    gtk_widget_set_sensitive(
        title_item,
        FALSE);

    gtk_menu_shell_append(
        menu_shell,
        title_item);

    gtk_menu_shell_append(
        menu_shell,
        gtk_separator_menu_item_new());

    g_free(window_title);

    gboolean hidden =
        atom_list_contains(
            x11->display,
            window,
            x11->atom_net_wm_state,
            x11->atom_hidden);

    GtkWidget *state =
        dock_x11_menu_item_new(
        hidden ?
        "Restore Window" :
        "Minimize Window",
        hidden ?
        "window-restore" :
        "window-minimize");

    g_object_set_data(
        G_OBJECT(state),
        "dock-group-window",
        (gpointer)(guintptr)window);

    g_object_set_data(
        G_OBJECT(state),
        "dock-x11",
        x11);

    g_signal_connect(
        state,
        "activate",
        G_CALLBACK(
            dock_x11_group_window_state_toggle),
        NULL);

    gtk_menu_shell_append(
        menu_shell,
        state);

    GtkWidget *move =
        dock_x11_menu_item_new(
            "Move to Workspace",
            "go-jump");

    GtkWidget *move_submenu =
        gtk_menu_new();

    guint32 current_desktop =
        get_current_desktop(x11);

    guint32 desktop_count =
        get_number_of_desktops(x11);

    if (desktop_count == 0 ||
        desktop_count == G_MAXUINT32) {
        GtkWidget *empty =
            gtk_menu_item_new_with_label(
                "Workspace information unavailable");

        gtk_widget_set_sensitive(
            empty,
            FALSE);

        gtk_menu_shell_append(
            GTK_MENU_SHELL(move_submenu),
            empty);
    } else {
        for (guint32 i = 0;
             i < desktop_count;
             i++) {
            gchar *label =
                g_strdup_printf(
                    "Workspace %u",
                    i + 1);

            GtkWidget *workspace =
                dock_x11_menu_item_new(
                label,
                "window-new");

            g_free(label);

            g_object_set_data(
                G_OBJECT(workspace),
                "dock-group-window",
                (gpointer)(guintptr)window);

            g_object_set_data(
                G_OBJECT(workspace),
                "dock-x11",
                x11);

            g_object_set_data(
                G_OBJECT(workspace),
                "dock-workspace",
                GUINT_TO_POINTER(i));

            if (current_desktop != G_MAXUINT32 &&
                current_desktop == i)
                gtk_widget_set_sensitive(
                    workspace,
                    FALSE);

            g_signal_connect(
                workspace,
                "activate",
                G_CALLBACK(
                    dock_x11_group_window_move_desktop),
                NULL);

            gtk_menu_shell_append(
                GTK_MENU_SHELL(move_submenu),
                workspace);
        }
    }

    gtk_menu_item_set_submenu(
        GTK_MENU_ITEM(move),
        move_submenu);

    gtk_menu_shell_append(
        menu_shell,
        move);

    gtk_menu_shell_append(
        menu_shell,
        gtk_separator_menu_item_new());

    GtkWidget *close =
        dock_x11_menu_item_new(
            "Close Window",
            "window-close");

    g_object_set_data(
        G_OBJECT(close),
        "dock-group-window",
        (gpointer)(guintptr)window);

    g_object_set_data(
        G_OBJECT(close),
        "dock-x11",
        x11);

    g_signal_connect(
        close,
        "activate",
        G_CALLBACK(
            dock_x11_close_group_window),
        NULL);

    gtk_menu_shell_append(
        menu_shell,
        close);
}

static guint32
get_number_of_desktops(
    DockX11 *x11)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        x11->display,
        x11->root,
        x11->atom_number_of_desktops,
        0,
        1,
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

        return G_MAXUINT32;
    }

    guint32 count = 0;

    gboolean read_ok =
        dock_x11_property32_read(
            (const unsigned long *)data,
            item_count,
            0,
            &count);

    XFree(data);

    return read_ok ? count : G_MAXUINT32;
}

static void
move_window_to_desktop(
    DockX11 *x11,
    Window window,
    guint32 desktop)
{
    if (!x11 ||
        window == None)
        return;

    XEvent event;
    memset(&event, 0, sizeof(event));

    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type =
        x11->atom_net_wm_desktop;
    event.xclient.format = 32;
    event.xclient.data.l[0] =
        desktop;
    event.xclient.data.l[1] =
        2;
    event.xclient.data.l[2] = 0;
    event.xclient.data.l[3] = 0;
    event.xclient.data.l[4] = 0;

    XSendEvent(
        x11->display,
        x11->root,
        False,
        SubstructureRedirectMask |
        SubstructureNotifyMask,
        &event);

    XFlush(x11->display);
}

static void
dock_x11_group_window_state_toggle(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    (void)user_data;

    DockX11 *x11 =
        g_object_get_data(
            G_OBJECT(menu_item),
            "dock-x11");

    Window window =
        (Window)(guintptr)g_object_get_data(
            G_OBJECT(menu_item),
            "dock-group-window");

    if (!x11 || window == None)
        return;

    gboolean hidden =
        atom_list_contains(
            x11->display,
            window,
            x11->atom_net_wm_state,
            x11->atom_hidden);

    if (hidden)
        activate_window(
            x11,
            window,
            gtk_get_current_event_time());
    else
        minimize_window(
            x11,
            window);
}

static void
dock_x11_group_window_move_desktop(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    (void)user_data;

    DockX11 *x11 =
        g_object_get_data(
            G_OBJECT(menu_item),
            "dock-x11");

    Window window =
        (Window)(guintptr)g_object_get_data(
            G_OBJECT(menu_item),
            "dock-group-window");

    guint32 desktop =
        GPOINTER_TO_UINT(
            g_object_get_data(
                G_OBJECT(menu_item),
                "dock-workspace"));

    if (!x11 ||
        window == None)
        return;

    move_window_to_desktop(
        x11,
        window,
        desktop);
}

static guint32
get_window_desktop(
    DockX11 *x11,
    Window window)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        x11->display,
        window,
        x11->atom_net_wm_desktop,
        0,
        1,
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
        return G_MAXUINT32;
    }

    guint32 desktop = 0;

    gboolean read_ok =
        dock_x11_property32_read(
            (const unsigned long *)data,
            item_count,
            0,
            &desktop);

    XFree(data);
    return read_ok ? desktop : G_MAXUINT32;
}

static guint32
get_current_desktop(
    DockX11 *x11)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        x11->display,
        x11->root,
        x11->atom_current_desktop,
        0,
        1,
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
        return G_MAXUINT32;
    }

    guint32 desktop = 0;

    gboolean read_ok =
        dock_x11_property32_read(
            (const unsigned long *)data,
            item_count,
            0,
            &desktop);

    XFree(data);
    return read_ok ? desktop : G_MAXUINT32;
}

static void
send_current_desktop(
    DockX11 *x11,
    guint32 desktop)
{
    XEvent event;
    memset(&event, 0, sizeof(event));

    event.xclient.type = ClientMessage;
    event.xclient.window = x11->root;
    event.xclient.message_type =
        x11->atom_current_desktop;
    event.xclient.format = 32;
    event.xclient.data.l[0] = desktop;
    event.xclient.data.l[1] =
        CurrentTime;

    XSendEvent(
        x11->display,
        x11->root,
        False,
        SubstructureRedirectMask |
        SubstructureNotifyMask,
        &event);
}

static void
remove_hidden_state(
    DockX11 *x11,
    Window window)
{
    XEvent event;
    memset(&event, 0, sizeof(event));

    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type =
        x11->atom_net_wm_state;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 0;
    event.xclient.data.l[1] = x11->atom_hidden;
    event.xclient.data.l[2] = 0;

    XSendEvent(
        x11->display,
        x11->root,
        False,
        SubstructureRedirectMask |
        SubstructureNotifyMask,
        &event);
}

static void
minimize_window(
    DockX11 *x11,
    Window window)
{
    XIconifyWindow(
        x11->display,
        window,
        DefaultScreen(x11->display));

    XFlush(x11->display);
}

static void
activate_window(
    DockX11 *x11,
    Window window,
    guint32 timestamp)
{
    guint32 window_desktop =
        get_window_desktop(x11, window);
    guint32 current_desktop =
        get_current_desktop(x11);

    if (window_desktop != G_MAXUINT32 &&
        window_desktop != 0xFFFFFFFFU &&
        current_desktop != G_MAXUINT32 &&
        window_desktop != current_desktop) {
        send_current_desktop(
            x11,
            window_desktop);
    }

    remove_hidden_state(
        x11,
        window);

    XEvent event;
    memset(&event, 0, sizeof(event));

    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type =
        x11->atom_active_window;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 1;
    event.xclient.data.l[1] =
        timestamp != 0 ? timestamp : CurrentTime;
    event.xclient.data.l[2] = None;

    XSendEvent(
        x11->display,
        x11->root,
        False,
        SubstructureRedirectMask |
        SubstructureNotifyMask,
        &event);

    XFlush(x11->display);
}


static void
dock_x11_update_button_tooltip(
    GtkWidget *button,
    DockLauncher *launcher,
    guint window_count,
    gboolean shortcut_only)
{
    if (!button ||
        !launcher)
        return;

    const gchar *app_name =
        dock_launcher_get_name(launcher);

    const gchar *name =
        app_name && *app_name ?
        app_name :
        "Application";

    gchar *tooltip = NULL;

    if (shortcut_only ||
        window_count == 0) {
        tooltip =
            g_strdup_printf(
                "%s\nLeft-click: launch\n"
                "Right-click: context menu",
                name);
    } else if (window_count == 1) {
        tooltip =
            g_strdup_printf(
                "%s\n1 window\n"
                "Left-click: activate/minimize\n"
                "Right-click: context menu",
                name);
    } else {
        tooltip =
            g_strdup_printf(
                "%s\n%u windows\n"
                "Left-click: activate/minimize\n"
                "Right-click: context menu",
                name,
                window_count);
    }

    gtk_widget_set_tooltip_text(
        button,
        tooltip);

    g_free(tooltip);
}

static void
dock_x11_refresh_button_with_matches(
    DockX11 *x11,
    GtkWidget *button,
    DockLauncher *launcher,
    GArray *matches)
{
    if (!x11 ||
        !GTK_IS_WIDGET(button) ||
        !launcher ||
        !matches)
        return;

    dock_x11_update_button_tooltip(
        button,
        launcher,
        matches->len,
        x11->shortcut_only);

    if (x11->shortcut_only) {
        g_object_set_data(
            G_OBJECT(button),
            "dock-running",
            GINT_TO_POINTER(matches->len > 0));

        g_object_set_data(
            G_OBJECT(button),
            "dock-x11-window-count",
            GUINT_TO_POINTER(matches->len));

        dock_icon_button_set_instance_count(
            button,
            0);

        gtk_widget_unset_state_flags(
            button,
            GTK_STATE_FLAG_ACTIVE);

        if (matches->len > 0)
            dock_icon_button_set_launching(
                button,
                FALSE);

        return;
    }

    gboolean active = FALSE;

    for (guint i = 0;
         i < matches->len;
         i++) {
        DockX11Match *match =
            &g_array_index(
                matches,
                DockX11Match,
                i);

        if (match->active) {
            active = TRUE;
            break;
        }
    }

    g_object_set_data(
        G_OBJECT(button),
        "dock-running",
        GINT_TO_POINTER(matches->len > 0));

    g_object_set_data(
        G_OBJECT(button),
        "dock-x11-window-count",
        GUINT_TO_POINTER(matches->len));

    if (matches->len > 0)
        dock_icon_button_set_launching(
            button,
            FALSE);

    dock_icon_button_set_instance_count(
        button,
        x11->window_indicator_enabled ?
        matches->len :
        0);

    if (active) {
        gtk_widget_set_state_flags(
            button,
            GTK_STATE_FLAG_ACTIVE,
            TRUE);
    } else {
        gtk_widget_unset_state_flags(
            button,
            GTK_STATE_FLAG_ACTIVE);
    }
}

void
dock_x11_refresh_button(
    DockX11 *x11,
    GtkWidget *button)
{
    if (!x11 ||
        !GTK_IS_WIDGET(button))
        return;

    DockLauncher *launcher =
        g_object_get_data(
            G_OBJECT(button),
            "dock-launcher");

    if (!launcher)
        return;

    GArray *matches =
        find_matches(
            x11,
            dock_launcher_get_app_info(launcher));

    dock_x11_refresh_button_with_matches(
        x11,
        button,
        launcher,
        matches);

    g_array_unref(matches);
}

static gboolean
dock_x11_refresh(gpointer user_data)
{
    DockX11 *x11 = user_data;

    if (x11->group_popup_suppressed &&
        x11->group_popup)
        dock_x11_group_popup_destroy(x11);

    if (!GDK_IS_X11_DISPLAY(
            gtk_widget_get_display(x11->container)))
        return G_SOURCE_CONTINUE;

    dock_x11_minimized_refresh(x11);
    dock_x11_minimized_group_popup_refresh_states(x11);

    if (x11->shortcut_only)
        return G_SOURCE_CONTINUE;

    GList *children =
        gtk_container_get_children(
            GTK_CONTAINER(x11->container));

    DockX11WindowSnapshot snapshot;
    dock_x11_window_snapshot_load(
        x11,
        &snapshot);

    for (GList *iter = children; iter; iter = iter->next) {
        GtkWidget *button =
            GTK_WIDGET(iter->data);

        DockLauncher *launcher =
            g_object_get_data(
                G_OBJECT(button),
                "dock-launcher");

        if (!launcher)
            continue;

        GArray *matches =
            find_matches_from_snapshot(
                x11,
                dock_launcher_get_app_info(launcher),
                &snapshot);

        dock_x11_refresh_button_with_matches(
            x11,
            button,
            launcher,
            matches);

        if (x11->group_popup &&
            x11->group_anchor == button &&
            matches->len >= 2) {
            Window active_window =
                snapshot.active;

            gboolean active_member =
                dock_x11_group_active_window_belongs(
                    matches,
                    active_window);

            if (active_member)
                x11->group_had_active_member = TRUE;

            /*
             * The expansion is transient, but opening an inactive group is
             * valid. Only collapse automatically after one of the group's
             * windows has actually been active and focus later moves outside
             * the group.
             */
            if (x11->group_pending_activation == None &&
                x11->group_had_active_member &&
                !active_member) {
                dock_x11_group_popup_close(x11);
            } else {
                dock_x11_group_popup_refresh_states(
                    x11,
                    matches);
            }

            if (x11->group_pending_activation != None) {
                gboolean pending_exists = FALSE;
                gboolean pending_active = FALSE;

                for (guint i = 0; i < matches->len; i++) {
                    DockX11Match *match =
                        &g_array_index(
                            matches,
                            DockX11Match,
                            i);

                    if (match->window ==
                        x11->group_pending_activation) {
                        pending_exists = TRUE;
                        pending_active = match->active;
                        break;
                    }
                }

                if (!pending_exists) {
                    x11->group_pending_activation = None;
                } else if (pending_active) {
                    x11->group_pending_activation = None;
                    dock_x11_group_popup_close(x11);
                }
            }
        }

        if (x11->group_popup &&
            x11->group_anchor == button &&
            matches->len < 2) {
            dock_x11_group_popup_close(x11);
        } else if (x11->group_popup &&
                   x11->group_anchor == button &&
                   matches->len != x11->group_total_window_count) {
            dock_x11_group_popup_close(x11);
        }

        g_array_unref(matches);
    }

    dock_x11_window_snapshot_clear(&snapshot);
    g_list_free(children);
    return G_SOURCE_CONTINUE;
}

gboolean
dock_x11_append_window_menu(
    DockX11 *x11,
    GtkWidget *button,
    GtkMenuShell *menu_shell)
{
    g_return_val_if_fail(
        x11 != NULL,
        FALSE);
    g_return_val_if_fail(
        GTK_IS_WIDGET(button),
        FALSE);
    g_return_val_if_fail(
        GTK_IS_MENU_SHELL(menu_shell),
        FALSE);

    if (x11->shortcut_only)
        return FALSE;

    DockLauncher *launcher =
        g_object_get_data(
            G_OBJECT(button),
            "dock-launcher");

    if (!launcher)
        return FALSE;

    GArray *matches =
        find_matches(
            x11,
            dock_launcher_get_app_info(launcher));

    if (matches->len == 0) {
        g_array_unref(matches);
        return FALSE;
    }

    if (matches->len == 1) {
        DockX11Match *match =
            &g_array_index(
                matches,
                DockX11Match,
                0);

        gtk_menu_shell_append(
            menu_shell,
            gtk_separator_menu_item_new());

        dock_x11_append_window_actions_menu(
            x11,
            match->window,
            menu_shell);

        g_array_unref(matches);
        return TRUE;
    }

    GtkWidget *windows =
        dock_x11_menu_item_new(
        "Windows",
        "view-list-details");

    GtkWidget *submenu =
        gtk_menu_new();

    gtk_menu_set_reserve_toggle_size(
        GTK_MENU(submenu),
        FALSE);

    for (guint i = 0; i < matches->len; i++) {
        DockX11Match *match =
            &g_array_index(
                matches,
                DockX11Match,
                i);

        gchar *title =
            get_window_title(
                x11,
                match->window);

        gchar *label = NULL;

        if (title && *title) {
            if (match->active)
                label =
                    g_strdup_printf(
                        "✓ %s",
                        title);
            else if (match->hidden)
                label =
                    g_strdup_printf(
                        "[Minimized] %s",
                        title);
            else
                label =
                    g_strdup(title);
        } else {
            label =
                g_strdup(
                    dock_launcher_get_name(
                        launcher));
        }

        GtkWidget *item =
            dock_x11_menu_item_new(
                label,
                "window-new");

        g_free(label);
        g_free(title);

        g_object_set_data(
            G_OBJECT(item),
            "dock-x11",
            x11);

        g_object_set_data(
            G_OBJECT(item),
            "dock-group-window",
            (gpointer)(guintptr)match->window);

        g_signal_connect(
            item,
            "activate",
            G_CALLBACK(dock_x11_activate_menu_window),
            NULL);

        GtkWidget *actions =
            gtk_menu_new();

    gtk_menu_set_reserve_toggle_size(
        GTK_MENU(actions),
        FALSE);

        dock_x11_append_window_actions_menu(
            x11,
            match->window,
            GTK_MENU_SHELL(actions));

        gtk_menu_item_set_submenu(
            GTK_MENU_ITEM(item),
            actions);

        gtk_menu_shell_append(
            GTK_MENU_SHELL(submenu),
            item);
    }

    gtk_menu_item_set_submenu(
        GTK_MENU_ITEM(windows),
        submenu);

    gtk_menu_shell_append(
        menu_shell,
        windows);

    g_array_unref(matches);
    return TRUE;
}

DockX11 *
dock_x11_new(GtkWidget *container)
{
    g_return_val_if_fail(
        GTK_IS_WIDGET(container),
        NULL);

    GdkDisplay *gdk_display =
        gtk_widget_get_display(container);

    if (!GDK_IS_X11_DISPLAY(gdk_display))
        return NULL;

    DockX11 *x11 =
        g_new0(DockX11, 1);

    dock_x11_install_error_handler();

    x11->container = container;
    x11->window_indicator_enabled = TRUE;
    x11->shortcut_only = FALSE;
    x11->minimized_window_icons = FALSE;
    x11->minimized_group_drawer = FALSE;
    x11->minimized_all_workspaces = FALSE;
    x11->icon_size = 48;
    x11->opacity_percent = 100;

    x11->display =
        gdk_x11_display_get_xdisplay(
            gdk_display);

    x11->root =
        DefaultRootWindow(x11->display);

    x11->atom_client_list =
        XInternAtom(
            x11->display,
            "_NET_CLIENT_LIST",
            False);

    x11->atom_active_window =
        XInternAtom(
            x11->display,
            "_NET_ACTIVE_WINDOW",
            False);

    x11->atom_current_desktop =
        XInternAtom(
            x11->display,
            "_NET_CURRENT_DESKTOP",
            False);

    x11->atom_number_of_desktops =
        XInternAtom(
            x11->display,
            "_NET_NUMBER_OF_DESKTOPS",
            False);

    x11->atom_net_wm_desktop =
        XInternAtom(
            x11->display,
            "_NET_WM_DESKTOP",
            False);

    x11->atom_net_wm_pid =
        XInternAtom(
            x11->display,
            "_NET_WM_PID",
            False);

    x11->atom_net_wm_name =
        XInternAtom(
            x11->display,
            "_NET_WM_NAME",
            False);

    x11->atom_utf8_string =
        XInternAtom(
            x11->display,
            "UTF8_STRING",
            False);

    x11->atom_net_wm_state =
        XInternAtom(
            x11->display,
            "_NET_WM_STATE",
            False);

    x11->atom_wm_protocols =
        XInternAtom(
            x11->display,
            "WM_PROTOCOLS",
            False);

    x11->atom_delete_window =
        XInternAtom(
            x11->display,
            "WM_DELETE_WINDOW",
            False);

    x11->atom_hidden =
        XInternAtom(
            x11->display,
            "_NET_WM_STATE_HIDDEN",
            False);

    x11->atom_net_wm_window_type =
        XInternAtom(
            x11->display,
            "_NET_WM_WINDOW_TYPE",
            False);

    x11->atom_dock_window_type =
        XInternAtom(
            x11->display,
            "_NET_WM_WINDOW_TYPE_DOCK",
            False);

    x11->atom_desktop_window_type =
        XInternAtom(
            x11->display,
            "_NET_WM_WINDOW_TYPE_DESKTOP",
            False);

    /*
     * The periodic reconciliation is intentionally small and cheap. It
     * catches launches, exits, minimized windows, and windows created by
     * D-Bus activation without requiring app-specific hooks.
     */
    x11->refresh_id =
        g_timeout_add(
            250,
            dock_x11_refresh,
            x11);

    dock_x11_refresh(x11);

    return x11;
}

void
dock_x11_free(DockX11 *x11)
{
    if (!x11)
        return;

    dock_x11_minimized_group_popup_destroy(x11);
    dock_x11_minimized_popup_destroy(x11);

    if (x11->refresh_id) {
        g_source_remove(x11->refresh_id);
        x11->refresh_id = 0;
    }

    dock_x11_group_popup_destroy(x11);

    dock_x11_uninstall_error_handler(
        x11->display);

    g_free(x11);
}

Window
dock_x11_get_active_window(
    DockX11 *x11)
{
    g_return_val_if_fail(
        x11 != NULL,
        None);

    return get_active_window(x11);
}

void
dock_x11_set_opacity(
    DockX11 *x11,
    gint opacity_percent)
{
    if (!x11)
        return;

    x11->opacity_percent =
        CLAMP(opacity_percent, 50, 100);

    if (x11->group_popup) {
        gtk_widget_set_opacity(
            x11->group_popup,
            x11->opacity_percent / 100.0);
    }

    if (x11->miniwindow_popup) {
        gtk_widget_set_opacity(
            x11->miniwindow_popup,
            x11->opacity_percent / 100.0);
    }
}

void
dock_x11_set_icon_size(
    DockX11 *x11,
    gint icon_size)
{
    if (!x11)
        return;

    x11->icon_size =
        CLAMP(icon_size, 16, 64);

    if (x11->minimized_window_icons &&
        x11->miniwindow_popup) {
        dock_x11_minimized_popup_destroy(x11);
        dock_x11_minimized_refresh(x11);
    }

    if (!x11->group_popup)
        return;

    GtkWidget *fixed =
        g_object_get_data(
            G_OBJECT(x11->group_popup),
            "dock-group-fixed");

    if (!fixed || !GTK_IS_FIXED(fixed))
        return;

    gint tile_size =
        dock_x11_tile_size(x11);

    gtk_window_resize(
        GTK_WINDOW(x11->group_popup),
        (gint)x11->group_window_count * tile_size,
        tile_size);

    GList *children =
        gtk_container_get_children(
            GTK_CONTAINER(fixed));

    for (GList *iter = children;
         iter;
         iter = iter->next) {
        GtkWidget *button =
            GTK_WIDGET(iter->data);

        gtk_widget_set_size_request(
            button,
            tile_size,
            tile_size);

        dock_icon_button_set_icon_size(
            button,
            x11->icon_size);
    }

    g_list_free(children);

    dock_x11_group_popup_position(
        x11,
        x11->group_animation_progress);
}

void
dock_x11_set_shortcut_only(
    DockX11 *x11,
    gboolean enabled)
{
    if (!x11)
        return;

    x11->shortcut_only = !!enabled;

    if (x11->shortcut_only) {
        dock_x11_group_popup_destroy(x11);
    } else {
        dock_x11_minimized_group_popup_destroy(x11);
        dock_x11_minimized_popup_destroy(x11);
    }

    dock_x11_set_window_indicator_enabled(
        x11,
        x11->window_indicator_enabled);

    dock_x11_minimized_refresh(x11);
}

void
dock_x11_set_minimized_window_icons(
    DockX11 *x11,
    gboolean enabled)
{
    if (!x11)
        return;

    enabled = !!enabled;

    if (x11->minimized_window_icons == enabled)
        return;

    x11->minimized_window_icons = enabled;

    if (!x11->minimized_window_icons)
        dock_x11_minimized_group_popup_destroy(x11);

    dock_x11_minimized_popup_destroy(x11);
    dock_x11_minimized_refresh(x11);
}

void
dock_x11_set_minimized_title_labels(
    DockX11 *x11,
    gboolean enabled)
{
    if (!x11)
        return;

    enabled = !!enabled;

    if (x11->minimized_title_labels == enabled)
        return;

    x11->minimized_title_labels = enabled;

    dock_x11_minimized_group_popup_destroy(x11);
    dock_x11_minimized_popup_destroy(x11);
    dock_x11_minimized_refresh(x11);
}

void
dock_x11_set_minimized_group_drawer(
    DockX11 *x11,
    gboolean enabled)
{
    if (!x11)
        return;

    x11->minimized_group_drawer = !!enabled;

    if (!x11->minimized_group_drawer)
        dock_x11_minimized_group_popup_destroy(x11);
}

void
dock_x11_set_minimized_all_workspaces(
    DockX11 *x11,
    gboolean enabled)
{
    if (!x11)
        return;

    enabled = !!enabled;

    if (x11->minimized_all_workspaces == enabled)
        return;

    x11->minimized_all_workspaces = enabled;
    dock_x11_minimized_group_popup_destroy(x11);
    dock_x11_minimized_popup_destroy(x11);
    dock_x11_minimized_refresh(x11);
}

void
dock_x11_set_window_indicator_enabled(
    DockX11 *x11,
    gboolean enabled)
{
    if (!x11)
        return;

    x11->window_indicator_enabled = !!enabled;

    if (!x11->container)
        return;

    GList *children =
        gtk_container_get_children(
            GTK_CONTAINER(x11->container));

    for (GList *iter = children;
         iter;
         iter = iter->next) {
        GtkWidget *button =
            GTK_WIDGET(iter->data);

        if (!g_object_get_data(
                G_OBJECT(button),
                "dock-launcher"))
            continue;

        dock_x11_refresh_button(
            x11,
            button);
    }

    g_list_free(children);
}

gboolean
dock_x11_activate_button(
    DockX11 *x11,
    GtkWidget *button,
    guint32 timestamp,
    Window active_before_click)
{
    g_return_val_if_fail(
        x11 != NULL,
        FALSE);

    g_return_val_if_fail(
        GTK_IS_WIDGET(button),
        FALSE);

    if (x11->shortcut_only)
        return FALSE;

    DockLauncher *launcher =
        g_object_get_data(
            G_OBJECT(button),
            "dock-launcher");

    if (!launcher)
        return FALSE;

    GArray *matches =
        find_matches(
            x11,
            dock_launcher_get_app_info(launcher));

    if (matches->len == 0) {
        if (x11->group_popup)
            dock_x11_group_popup_close(x11);

        g_array_unref(matches);
        return FALSE;
    }

    if (matches->len == 1) {
        if (x11->group_popup)
            dock_x11_group_popup_destroy(x11);

        DockX11Match *match =
            &g_array_index(
                matches,
                DockX11Match,
                0);

        if (match->active ||
            match->window == active_before_click) {
            minimize_window(
                x11,
                match->window);

            g_object_set_data(
                G_OBJECT(button),
                "dock-running",
                GINT_TO_POINTER(TRUE));

            gtk_widget_unset_state_flags(
                button,
                GTK_STATE_FLAG_ACTIVE);
        } else {
            activate_window(
                x11,
                match->window,
                timestamp);

            g_object_set_data(
                G_OBJECT(button),
                "dock-running",
                GINT_TO_POINTER(TRUE));

            gtk_widget_set_state_flags(
                button,
                GTK_STATE_FLAG_ACTIVE,
                TRUE);
        }

        g_array_unref(matches);
        return TRUE;
    }

    /*
     * A multi-window application expands into a horizontal group. The
     * original 64x64 icon remains the anchor; the other windows slide out
     * from its left side as individual 64x64 blocks.
     */
    if (x11->group_popup_suppressed) {
        g_array_unref(matches);
        return TRUE;
    }

    if (x11->group_popup) {
        if (x11->group_anchor == button) {
            dock_x11_group_popup_close(x11);
            g_array_unref(matches);
            return TRUE;
        }

        dock_x11_group_popup_destroy(x11);
    }

    if (!dock_x11_group_popup_open(
            x11,
            button,
            launcher,
            matches)) {
        g_array_unref(matches);
        return FALSE;
    }

    g_array_unref(matches);
    return TRUE;
}

