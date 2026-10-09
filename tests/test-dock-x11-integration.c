/*
 * End-to-end X11 window-tracking tests.
 *
 * The harness publishes a small EWMH client list on an isolated Xvfb server.
 * It exercises the real DockX11 tracker and menu-building path, without a
 * window manager or any changes to the developer's desktop session.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <gio/gdesktopappinfo.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <string.h>
#include <unistd.h>

#include "dock-icon.h"
#include "dock-launcher.h"
#include "dock-x11.h"

typedef struct {
    Display *display;
    Window root;
    Window first;
    Window second;

    Atom atom_client_list;
    Atom atom_active_window;
    Atom atom_current_desktop;
    Atom atom_number_of_desktops;
    Atom atom_net_wm_desktop;
    Atom atom_net_wm_pid;
    Atom atom_net_wm_window_type;
    Atom atom_net_wm_name;
    Atom atom_utf8_string;
    Atom atom_window_type_normal;

    GtkWidget *main_window;
    GtkWidget *container;
    GtkWidget *button;
    DockLauncher *launcher;
    DockX11 *tracker;
} X11IntegrationFixture;

static X11IntegrationFixture fixture;
static gchar *test_data_home;
static gchar *test_config_home;
static gchar *test_desktop_path;

#define TEST_DESKTOP_ID "crepido-integration-test.desktop"
#define TEST_WM_CLASS "CrepidoIntegrationApp"
#define TEST_WINDOW_ONE_TITLE "Test Window One"
#define TEST_WINDOW_TWO_TITLE "Test Window Two"

static void
set_cardinal_property(
    Window window,
    Atom property,
    guint32 value)
{
    unsigned long native_value = (unsigned long)value;

    XChangeProperty(
        fixture.display,
        window,
        property,
        XA_CARDINAL,
        32,
        PropModeReplace,
        (unsigned char *)&native_value,
        1);
}

static void
set_active_window(Window window)
{
    unsigned long native_window = (unsigned long)window;

    XChangeProperty(
        fixture.display,
        fixture.root,
        fixture.atom_active_window,
        XA_WINDOW,
        32,
        PropModeReplace,
        (unsigned char *)&native_window,
        1);

    XSync(fixture.display, False);
}

static void
set_current_desktop(guint32 desktop)
{
    set_cardinal_property(
        fixture.root,
        fixture.atom_current_desktop,
        desktop);

    XSync(fixture.display, False);
}

static void
set_window_desktop(
    Window window,
    guint32 desktop)
{
    set_cardinal_property(
        window,
        fixture.atom_net_wm_desktop,
        desktop);
}

static void
set_client_list(
    const Window *windows,
    gsize count)
{
    if (!windows || count == 0) {
        XDeleteProperty(
            fixture.display,
            fixture.root,
            fixture.atom_client_list);
    } else {
        XChangeProperty(
            fixture.display,
            fixture.root,
            fixture.atom_client_list,
            XA_WINDOW,
            32,
            PropModeReplace,
            (const unsigned char *)windows,
            (int)count);
    }

    XSync(fixture.display, False);
}

static void
set_window_title(
    Window window,
    const gchar *title)
{
    XChangeProperty(
        fixture.display,
        window,
        fixture.atom_net_wm_name,
        fixture.atom_utf8_string,
        8,
        PropModeReplace,
        (const unsigned char *)title,
        (int)strlen(title));

    XStoreName(
        fixture.display,
        window,
        title);
}

static Window
create_test_window(
    const gchar *title,
    guint32 desktop)
{
    Window window =
        XCreateSimpleWindow(
            fixture.display,
            fixture.root,
            20,
            20,
            180,
            100,
            0,
            BlackPixel(
                fixture.display,
                DefaultScreen(fixture.display)),
            WhitePixel(
                fixture.display,
                DefaultScreen(fixture.display)));

    g_assert_true(window != None);

    XClassHint class_hint = {
        .res_name = (char *)"crepido-integration-test",
        .res_class = (char *)TEST_WM_CLASS
    };

    g_assert_true(
        XSetClassHint(
            fixture.display,
            window,
            &class_hint) != 0);

    set_window_title(window, title);
    set_window_desktop(window, desktop);
    set_cardinal_property(
        window,
        fixture.atom_net_wm_pid,
        (guint32)getpid());

    Atom window_type = fixture.atom_window_type_normal;

    XChangeProperty(
        fixture.display,
        window,
        fixture.atom_net_wm_window_type,
        XA_ATOM,
        32,
        PropModeReplace,
        (const unsigned char *)&window_type,
        1);

    XMapWindow(fixture.display, window);
    XSync(fixture.display, False);

    return window;
}

static gchar *
find_first_label_text(GtkWidget *widget)
{
    if (GTK_IS_LABEL(widget))
        return g_strdup(
            gtk_label_get_text(GTK_LABEL(widget)));

    if (!GTK_IS_CONTAINER(widget))
        return NULL;

    GList *children =
        gtk_container_get_children(
            GTK_CONTAINER(widget));

    gchar *found = NULL;

    for (GList *iter = children; iter; iter = iter->next) {
        found = find_first_label_text(
            GTK_WIDGET(iter->data));

        if (found)
            break;
    }

    g_list_free(children);
    return found;
}

static gboolean
menu_label_matches(
    const gchar *label,
    const gchar *wanted,
    gboolean suffix_match)
{
    if (!label || !wanted)
        return FALSE;

    if (!suffix_match)
        return g_strcmp0(label, wanted) == 0;

    gsize label_length = strlen(label);
    gsize wanted_length = strlen(wanted);

    return label_length >= wanted_length &&
           strcmp(
               label + label_length - wanted_length,
               wanted) == 0;
}

static GtkWidget *
find_menu_item(
    GtkWidget *menu,
    const gchar *wanted_label,
    gboolean suffix_match)
{
    if (!menu || !GTK_IS_CONTAINER(menu))
        return NULL;

    GList *children =
        gtk_container_get_children(
            GTK_CONTAINER(menu));

    GtkWidget *found = NULL;

    for (GList *iter = children; iter; iter = iter->next) {
        GtkWidget *child = GTK_WIDGET(iter->data);

        if (!GTK_IS_MENU_ITEM(child))
            continue;

        gchar *label = find_first_label_text(child);

        gboolean matches =
            menu_label_matches(
                label,
                wanted_label,
                suffix_match);

        g_free(label);

        if (matches) {
            found = child;
            break;
        }
    }

    g_list_free(children);
    return found;
}

static void
collect_widget_labels(
    GtkWidget *widget,
    GPtrArray *labels)
{
    if (GTK_IS_LABEL(widget)) {
        g_ptr_array_add(
            labels,
            g_strdup(
                gtk_label_get_text(
                    GTK_LABEL(widget))));
        return;
    }

    if (GTK_IS_CONTAINER(widget)) {
        GList *children =
            gtk_container_get_children(
                GTK_CONTAINER(widget));

        for (GList *iter = children; iter; iter = iter->next) {
            collect_widget_labels(
                GTK_WIDGET(iter->data),
                labels);
        }

        g_list_free(children);
    }

    /*
     * GtkMenuItem submenus are managed separately from their label child,
     * so visit them explicitly when collecting the entire menu tree.
     */
    if (GTK_IS_MENU_ITEM(widget)) {
        GtkWidget *submenu =
            gtk_menu_item_get_submenu(
                GTK_MENU_ITEM(widget));

        if (submenu)
            collect_widget_labels(
                submenu,
                labels);
    }
}

static gboolean
label_array_contains(
    GPtrArray *labels,
    const gchar *wanted)
{
    for (guint i = 0; i < labels->len; i++) {
        if (g_strcmp0(
                g_ptr_array_index(labels, i),
                wanted) == 0)
            return TRUE;
    }

    return FALSE;
}

static void
assert_active_window_in_menu(
    const gchar *active_title,
    const gchar *inactive_title)
{
    GtkWidget *menu = gtk_menu_new();

    g_assert_true(
        dock_x11_append_window_menu(
            fixture.tracker,
            fixture.button,
            GTK_MENU_SHELL(menu)));

    GPtrArray *labels =
        g_ptr_array_new_with_free_func(g_free);

    collect_widget_labels(menu, labels);

    gchar *active_label =
        g_strdup_printf(
            "✓ %s",
            active_title);

    g_assert_true(
        label_array_contains(
            labels,
            active_label));

    gchar *inactive_label =
        g_strdup_printf(
            "✓ %s",
            inactive_title);

    g_assert_false(
        label_array_contains(
            labels,
            inactive_label));

    g_free(inactive_label);
    g_free(active_label);
    g_ptr_array_unref(labels);
    gtk_widget_destroy(menu);
}

static void
assert_workspace_menu_sensitivity(
    gboolean workspace_one_is_current)
{
    GtkWidget *menu = gtk_menu_new();

    g_assert_true(
        dock_x11_append_window_menu(
            fixture.tracker,
            fixture.button,
            GTK_MENU_SHELL(menu)));

    GtkWidget *windows_item =
        find_menu_item(
            menu,
            "Windows",
            FALSE);
    g_assert_nonnull(windows_item);

    GtkWidget *window_list =
        gtk_menu_item_get_submenu(
            GTK_MENU_ITEM(windows_item));
    g_assert_nonnull(window_list);

    GtkWidget *window_item =
        find_menu_item(
            window_list,
            TEST_WINDOW_ONE_TITLE,
            TRUE);
    g_assert_nonnull(window_item);

    GtkWidget *actions =
        gtk_menu_item_get_submenu(
            GTK_MENU_ITEM(window_item));
    g_assert_nonnull(actions);

    GtkWidget *move_item =
        find_menu_item(
            actions,
            "Move to Workspace",
            FALSE);
    g_assert_nonnull(move_item);

    GtkWidget *workspace_menu =
        gtk_menu_item_get_submenu(
            GTK_MENU_ITEM(move_item));
    g_assert_nonnull(workspace_menu);

    GtkWidget *workspace_one =
        find_menu_item(
            workspace_menu,
            "Workspace 1",
            FALSE);
    GtkWidget *workspace_two =
        find_menu_item(
            workspace_menu,
            "Workspace 2",
            FALSE);

    g_assert_nonnull(workspace_one);
    g_assert_nonnull(workspace_two);

    g_assert_cmpint(
        gtk_widget_get_sensitive(workspace_one),
        ==,
        !workspace_one_is_current);
    g_assert_cmpint(
        gtk_widget_get_sensitive(workspace_two),
        ==,
        workspace_one_is_current);

    gtk_widget_destroy(menu);
}

static gboolean
widget_tree_has_tooltip(
    GtkWidget *widget,
    const gchar *expected_tooltip)
{
    const gchar *tooltip =
        gtk_widget_get_tooltip_text(widget);

    if (g_strcmp0(tooltip, expected_tooltip) == 0)
        return TRUE;

    if (!GTK_IS_CONTAINER(widget))
        return FALSE;

    GList *children =
        gtk_container_get_children(
            GTK_CONTAINER(widget));

    gboolean found = FALSE;

    for (GList *iter = children; iter; iter = iter->next) {
        if (widget_tree_has_tooltip(
                GTK_WIDGET(iter->data),
                expected_tooltip)) {
            found = TRUE;
            break;
        }
    }

    g_list_free(children);
    return found;
}

static gboolean
any_toplevel_has_tooltip(const gchar *expected_tooltip)
{
    GList *toplevels = gtk_window_list_toplevels();
    gboolean found = FALSE;

    for (GList *iter = toplevels; iter; iter = iter->next) {
        if (widget_tree_has_tooltip(
                GTK_WIDGET(iter->data),
                expected_tooltip)) {
            found = TRUE;
            break;
        }
    }

    g_list_free(toplevels);
    return found;
}

static guint
button_window_count(void)
{
    return GPOINTER_TO_UINT(
        g_object_get_data(
            G_OBJECT(fixture.button),
            "dock-x11-window-count"));
}

static void
test_real_tracker_grouping_workspace_and_lifecycle(void)
{
    fixture.display = XOpenDisplay(NULL);
    g_assert_nonnull(fixture.display);

    fixture.root =
        DefaultRootWindow(fixture.display);

    fixture.atom_client_list =
        XInternAtom(
            fixture.display,
            "_NET_CLIENT_LIST",
            False);
    fixture.atom_active_window =
        XInternAtom(
            fixture.display,
            "_NET_ACTIVE_WINDOW",
            False);
    fixture.atom_current_desktop =
        XInternAtom(
            fixture.display,
            "_NET_CURRENT_DESKTOP",
            False);
    fixture.atom_number_of_desktops =
        XInternAtom(
            fixture.display,
            "_NET_NUMBER_OF_DESKTOPS",
            False);
    fixture.atom_net_wm_desktop =
        XInternAtom(
            fixture.display,
            "_NET_WM_DESKTOP",
            False);
    fixture.atom_net_wm_pid =
        XInternAtom(
            fixture.display,
            "_NET_WM_PID",
            False);
    fixture.atom_net_wm_window_type =
        XInternAtom(
            fixture.display,
            "_NET_WM_WINDOW_TYPE",
            False);
    fixture.atom_net_wm_name =
        XInternAtom(
            fixture.display,
            "_NET_WM_NAME",
            False);
    fixture.atom_utf8_string =
        XInternAtom(
            fixture.display,
            "UTF8_STRING",
            False);
    fixture.atom_window_type_normal =
        XInternAtom(
            fixture.display,
            "_NET_WM_WINDOW_TYPE_NORMAL",
            False);

    fixture.first =
        create_test_window(
            TEST_WINDOW_ONE_TITLE,
            0);
    fixture.second =
        create_test_window(
            TEST_WINDOW_TWO_TITLE,
            1);

    set_cardinal_property(
        fixture.root,
        fixture.atom_number_of_desktops,
        2);
    set_current_desktop(0);
    set_active_window(fixture.first);

    const Window clients[] = {
        fixture.first,
        fixture.second
    };

    set_client_list(
        clients,
        G_N_ELEMENTS(clients));

    fixture.launcher =
        dock_launcher_new_from_desktop_id(
            TEST_DESKTOP_ID);
    g_assert_nonnull(fixture.launcher);

    fixture.main_window =
        gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(
        GTK_WINDOW(fixture.main_window),
        160,
        100);

    fixture.container = gtk_fixed_new();

    gtk_container_add(
        GTK_CONTAINER(fixture.main_window),
        fixture.container);

    fixture.button =
        dock_icon_button_new(fixture.launcher);

    gtk_fixed_put(
        GTK_FIXED(fixture.container),
        fixture.button,
        12,
        12);

    gtk_widget_show_all(fixture.main_window);

    while (gtk_events_pending())
        gtk_main_iteration();

    /*
     * This is the real tracker used by the Dock, not a test double. The
     * fixture only supplies the EWMH properties normally published by a WM.
     */
    fixture.tracker =
        dock_x11_new(fixture.container);
    g_assert_nonnull(fixture.tracker);

    dock_x11_refresh_button(
        fixture.tracker,
        fixture.button);

    g_assert_cmpuint(button_window_count(), ==, 2);
    g_assert_true(
        (gtk_widget_get_state_flags(fixture.button) &
         GTK_STATE_FLAG_ACTIVE) != 0);

    assert_active_window_in_menu(
        TEST_WINDOW_ONE_TITLE,
        TEST_WINDOW_TWO_TITLE);

    /* The current workspace is insensitive in the move-to menu. */
    assert_workspace_menu_sensitivity(TRUE);

    /*
     * Switching the EWMH active-window property must move the checkmark to
     * the other member of the same launcher group.
     */
    set_current_desktop(1);
    set_active_window(fixture.second);

    dock_x11_refresh_button(
        fixture.tracker,
        fixture.button);

    g_assert_cmpuint(button_window_count(), ==, 2);
    g_assert_true(
        (gtk_widget_get_state_flags(fixture.button) &
         GTK_STATE_FLAG_ACTIVE) != 0);

    assert_active_window_in_menu(
        TEST_WINDOW_TWO_TITLE,
        TEST_WINDOW_ONE_TITLE);
    assert_workspace_menu_sensitivity(FALSE);

    /*
     * Opening a real group popup exercises per-window desktop parsing: the
     * non-active window's tooltip should contain its EWMH workspace number.
     */
    set_current_desktop(0);
    set_active_window(fixture.first);

    dock_x11_refresh_button(
        fixture.tracker,
        fixture.button);

    g_assert_true(
        dock_x11_activate_button(
            fixture.tracker,
            fixture.button,
            CurrentTime,
            fixture.first));

    g_assert_true(
        any_toplevel_has_tooltip(
            "Test Window Two — Workspace 2"));

    dock_x11_close_group_popup(fixture.tracker);

    /*
     * Simulate a client disappearing between a root client-list update and
     * property reads. The stale snapshot is rejected, and the next refresh
     * must recover when the EWMH list is corrected.
     */
    XDestroyWindow(
        fixture.display,
        fixture.second);
    XSync(fixture.display, False);
    fixture.second = None;

    dock_x11_refresh_button(
        fixture.tracker,
        fixture.button);

    g_assert_cmpuint(button_window_count(), ==, 0);

    const Window remaining[] = {
        fixture.first
    };

    set_client_list(
        remaining,
        G_N_ELEMENTS(remaining));

    dock_x11_refresh_button(
        fixture.tracker,
        fixture.button);

    g_assert_cmpuint(button_window_count(), ==, 1);

    set_client_list(NULL, 0);
    set_active_window(None);

    XDestroyWindow(
        fixture.display,
        fixture.first);
    XSync(fixture.display, False);
    fixture.first = None;

    dock_x11_refresh_button(
        fixture.tracker,
        fixture.button);

    g_assert_cmpuint(button_window_count(), ==, 0);

    dock_x11_free(fixture.tracker);
    fixture.tracker = NULL;

    gtk_widget_destroy(fixture.main_window);
    fixture.main_window = NULL;
    fixture.container = NULL;
    fixture.button = NULL;

    dock_launcher_free(fixture.launcher);
    fixture.launcher = NULL;

    XDeleteProperty(
        fixture.display,
        fixture.root,
        fixture.atom_client_list);
    XDeleteProperty(
        fixture.display,
        fixture.root,
        fixture.atom_active_window);
    XDeleteProperty(
        fixture.display,
        fixture.root,
        fixture.atom_current_desktop);
    XDeleteProperty(
        fixture.display,
        fixture.root,
        fixture.atom_number_of_desktops);

    XSync(fixture.display, False);
    XCloseDisplay(fixture.display);
    fixture.display = NULL;
}

static void
cleanup_test_data(void)
{
    if (test_desktop_path) {
        g_remove(test_desktop_path);
        g_free(test_desktop_path);
        test_desktop_path = NULL;
    }

    if (test_data_home) {
        gchar *applications =
            g_build_filename(
                test_data_home,
                "applications",
                NULL);

        g_rmdir(applications);
        g_free(applications);

        g_free(test_data_home);
        test_data_home = NULL;
    }

    if (test_config_home) {
        g_rmdir(test_config_home);
        g_free(test_config_home);
        test_config_home = NULL;
    }
}

static gboolean
prepare_test_desktop_file(void)
{
    GError *error = NULL;

    test_data_home =
        g_dir_make_tmp(
            "crepido-x11-integration-XXXXXX",
            &error);

    if (!test_data_home) {
        g_printerr(
            "Unable to create integration-test data directory: %s\n",
            error ? error->message : "unknown error");
        g_clear_error(&error);
        return FALSE;
    }

    g_setenv(
        "XDG_DATA_HOME",
        test_data_home,
        TRUE);

    test_config_home =
        g_build_filename(
            test_data_home,
            "config",
            NULL);

    g_setenv(
        "XDG_CONFIG_HOME",
        test_config_home,
        TRUE);

    gchar *applications =
        g_build_filename(
            test_data_home,
            "applications",
            NULL);

    if (g_mkdir_with_parents(
            applications,
            0700) != 0) {
        g_printerr(
            "Unable to create integration-test applications directory\n");
        g_free(applications);
        return FALSE;
    }

    test_desktop_path =
        g_build_filename(
            applications,
            TEST_DESKTOP_ID,
            NULL);

    const gchar *desktop_contents =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Crepido X11 Integration Test\n"
        "Exec=/bin/true\n"
        "Icon=application-x-executable\n"
        "StartupWMClass=" TEST_WM_CLASS "\n"
        "Terminal=false\n";

    gboolean written =
        g_file_set_contents(
            test_desktop_path,
            desktop_contents,
            -1,
            &error);

    if (!written) {
        g_printerr(
            "Unable to create integration-test desktop entry: %s\n",
            error ? error->message : "unknown error");
        g_clear_error(&error);
    }

    g_free(applications);
    return written;
}

int
main(int argc, char **argv)
{
    /*
     * This process runs inside Xvfb without a desktop accessibility bus.
     * Avoid a GTK AT-SPI warning being treated as fatal by g_test.
     */
    g_setenv("NO_AT_BRIDGE", "1", TRUE);

    g_test_init(&argc, &argv, NULL);

    if (!prepare_test_desktop_file()) {
        cleanup_test_data();
        return 1;
    }

    if (!gtk_init_check(&argc, &argv)) {
        g_printerr(
            "The X11 integration test requires a display; run it via xvfb-run.\n");
        cleanup_test_data();
        return 1;
    }

    g_test_add_func(
        "/x11-integration/group-active-workspace-lifecycle",
        test_real_tracker_grouping_workspace_and_lifecycle);

    gint result = g_test_run();

    cleanup_test_data();
    return result;
}
