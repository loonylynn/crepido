/*
 * Crepido configuration regression tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>

#include "dock-config.h"

static gchar *test_config_home = NULL;

static gchar *
test_config_directory(void)
{
    return g_build_filename(
        g_get_user_config_dir(),
        "crepido",
        NULL);
}

static gchar *
test_config_path(void)
{
    gchar *directory = test_config_directory();
    gchar *path = g_build_filename(directory, "dock.conf", NULL);
    g_free(directory);
    return path;
}

static void
test_reset_config(void)
{
    gchar *path = test_config_path();
    gchar *directory = test_config_directory();

    g_remove(path);
    g_rmdir(directory);

    g_free(path);
    g_free(directory);
}

static void
test_write_config(const gchar *contents)
{
    gchar *directory = test_config_directory();
    gchar *path = test_config_path();
    GError *error = NULL;

    g_assert_cmpint(
        g_mkdir_with_parents(directory, 0700),
        ==,
        0);

    g_assert_true(
        g_file_set_contents(path, contents, -1, &error));
    g_assert_no_error(error);

    g_free(path);
    g_free(directory);
}

static void
test_missing_config_is_not_an_error(void)
{
    test_reset_config();

    gboolean found = TRUE;
    gsize length = 99;
    GError *error = NULL;

    gchar **items =
        dock_config_load_items(&found, &length, &error);

    g_assert_no_error(error);
    g_assert_null(items);
    g_assert_false(found);
    g_assert_cmpuint(length, ==, 0);
}

static void
test_malformed_config_reports_error(void)
{
    test_reset_config();
    test_write_config("[Dock\nItems=app:broken.desktop\n");

    gboolean found = TRUE;
    gsize length = 99;
    GError *error = NULL;

    gchar **items =
        dock_config_load_items(&found, &length, &error);

    g_assert_null(items);
    g_assert_false(found);
    g_assert_cmpuint(length, ==, 0);
    g_assert_nonnull(error);
    g_clear_error(&error);

    test_reset_config();
}

static void
test_legacy_applications_are_migrated_in_memory(void)
{
    test_reset_config();
    test_write_config(
        "[Dock]\n"
        "Applications=firefox.desktop;mate-terminal.desktop;\n");

    gboolean found = FALSE;
    gsize length = 0;
    GError *error = NULL;

    gchar **items =
        dock_config_load_items(&found, &length, &error);

    g_assert_no_error(error);
    g_assert_true(found);
    g_assert_cmpuint(length, ==, 2);
    g_assert_nonnull(items);
    g_assert_cmpstr(items[0], ==, "app:firefox.desktop");
    g_assert_cmpstr(items[1], ==, "app:mate-terminal.desktop");

    g_strfreev(items);
    test_reset_config();
}

static void
test_full_state_round_trip(void)
{
    test_reset_config();

    const gchar *items[] = {
        "app:firefox.desktop",
        "drawer:0",
        "app:org.mate.Terminal.desktop"
    };

    const gchar *drawer_apps[] = {
        "org.mate.Terminal.desktop",
        "caja.desktop"
    };

    DockConfigDrawer drawer = {
        .name = (gchar *)"Utilities",
        .applications = (gchar **)drawer_apps,
        .application_count = G_N_ELEMENTS(drawer_apps)
    };

    GError *error = NULL;

    g_assert_true(
        dock_config_save_state(
            items,
            G_N_ELEMENTS(items),
            &drawer,
            1,
            TRUE,   /* right side */
            1,      /* auto raise/lower */
            1,      /* monitor index */
            "Dell Inc.",
            "U2720Q",
            56,     /* icon size */
            85,     /* opacity */
            TRUE,   /* running-window indicator */
            TRUE,   /* hide/reveal button */
            FALSE,  /* shortcut-only */
            TRUE,   /* minimized-window icons */
            TRUE,   /* minimized title labels */
            FALSE,  /* minimized group Drawer */
            TRUE,   /* all workspaces */
            TRUE,   /* bitmap backgrounds */
            1,      /* extend */
            "/tmp/crepido-test-background.png",
            &error));
    g_assert_no_error(error);

    gboolean found = FALSE;
    gsize length = 0;
    gchar **loaded_items =
        dock_config_load_items(&found, &length, &error);

    g_assert_no_error(error);
    g_assert_true(found);
    g_assert_cmpuint(length, ==, G_N_ELEMENTS(items));
    g_assert_nonnull(loaded_items);

    for (gsize i = 0; i < G_N_ELEMENTS(items); i++)
        g_assert_cmpstr(loaded_items[i], ==, items[i]);

    g_strfreev(loaded_items);

    gsize drawer_count = 0;
    DockConfigDrawer *loaded_drawers =
        dock_config_load_drawers(&drawer_count, &error);

    g_assert_no_error(error);
    g_assert_cmpuint(drawer_count, ==, 1);
    g_assert_nonnull(loaded_drawers);
    g_assert_cmpstr(loaded_drawers[0].name, ==, "Utilities");
    g_assert_cmpuint(
        loaded_drawers[0].application_count,
        ==,
        G_N_ELEMENTS(drawer_apps));
    g_assert_cmpstr(
        loaded_drawers[0].applications[0],
        ==,
        drawer_apps[0]);
    g_assert_cmpstr(
        loaded_drawers[0].applications[1],
        ==,
        drawer_apps[1]);

    dock_config_free_drawers(loaded_drawers, drawer_count);

    found = FALSE;
    g_assert_true(dock_config_load_side(&found, &error));
    g_assert_no_error(error);
    g_assert_true(found);

    found = FALSE;
    g_assert_cmpint(dock_config_load_position(&found, &error), ==, 1);
    g_assert_no_error(error);
    g_assert_true(found);

    found = FALSE;
    g_assert_cmpint(dock_config_load_monitor(&found, &error), ==, 1);
    g_assert_no_error(error);
    g_assert_true(found);

    found = FALSE;
    g_assert_cmpint(dock_config_load_icon_size(&found, &error), ==, 56);
    g_assert_no_error(error);
    g_assert_true(found);

    found = FALSE;
    g_assert_cmpint(dock_config_load_opacity(&found, &error), ==, 85);
    g_assert_no_error(error);
    g_assert_true(found);

    found = FALSE;
    g_assert_true(
        dock_config_load_bitmap_background_enabled(&found, &error));
    g_assert_no_error(error);
    g_assert_true(found);

    found = FALSE;
    g_assert_cmpint(
        dock_config_load_bitmap_background_mode(&found, &error),
        ==,
        1);
    g_assert_no_error(error);
    g_assert_true(found);

    found = FALSE;
    gchar *background_path =
        dock_config_load_bitmap_background_path(&found, &error);
    g_assert_no_error(error);
    g_assert_true(found);
    g_assert_cmpstr(
        background_path,
        ==,
        "/tmp/crepido-test-background.png");
    g_free(background_path);

    found = FALSE;
    g_assert_true(dock_config_load_show_hide_handle(&found, &error));
    g_assert_no_error(error);
    g_assert_true(found);

    test_reset_config();
}


static void
test_scalar_loader_defaults_and_validation(void)
{
    test_reset_config();

    gboolean found = TRUE;
    GError *error = NULL;

    g_assert_cmpint(
        dock_config_load_icon_size(&found, &error),
        ==,
        48);
    g_assert_no_error(error);
    g_assert_false(found);

    found = TRUE;
    g_assert_true(
        dock_config_load_window_indicator(&found, &error));
    g_assert_no_error(error);
    g_assert_false(found);

    test_write_config(
        "[Dock]\n"
        "IconSize=not-an-integer\n");

    GError *invalid_error = NULL;
    found = TRUE;
    gint invalid_icon_size =
        dock_config_load_icon_size(&found, &invalid_error);
    g_assert_cmpint(invalid_icon_size, ==, 48);
    g_assert_false(found);
    g_assert_nonnull(invalid_error);
    g_clear_error(&invalid_error);
    g_assert_no_error(error);

    test_reset_config();
    test_write_config(
        "[Dock]\n"
        "IconSize=128\n"
        "Opacity=20\n"
        "WindowIndicator=false\n"
        "BitmapBackgroundMode=2\n");

    found = FALSE;
    g_assert_cmpint(
        dock_config_load_icon_size(&found, &error),
        ==,
        64);
    g_assert_no_error(error);
    g_assert_true(found);

    found = FALSE;
    g_assert_cmpint(
        dock_config_load_opacity(&found, &error),
        ==,
        50);
    g_assert_no_error(error);
    g_assert_true(found);

    found = FALSE;
    g_assert_false(
        dock_config_load_window_indicator(&found, &error));
    g_assert_no_error(error);
    g_assert_true(found);

    found = FALSE;
    g_assert_cmpint(
        dock_config_load_bitmap_background_mode(&found, &error),
        ==,
        0);
    g_assert_no_error(error);
    g_assert_true(found);

    test_reset_config();
}


static void
test_snapshot_missing_config_defaults(void)
{
    test_reset_config();

    DockConfigState state = {0};
    GError *error = NULL;

    g_assert_false(dock_config_load_state(&state, &error));
    g_assert_no_error(error);
    g_assert_cmpint(state.monitor_index, ==, -1);
    g_assert_cmpint(state.icon_size, ==, 48);
    g_assert_cmpint(state.opacity, ==, 100);
    g_assert_true(state.on_right_side);
    g_assert_true(state.show_hide_handle);
    g_assert_false(state.items_found);
    g_assert_null(state.items);

    dock_config_state_clear(&state);
}

static void
test_snapshot_keeps_valid_settings_when_one_is_malformed(void)
{
    test_reset_config();
    test_write_config(
        "[Dock]\n"
        "IconSize=not-an-integer\n"
        "Opacity=82\n"
        "WindowIndicator=false\n"
        "Items=app:firefox.desktop;\n");

    DockConfigState state = {0};
    GError *error = NULL;

    g_assert_true(dock_config_load_state(&state, &error));
    g_assert_no_error(error);

    g_assert_cmpint(state.icon_size, ==, 48);
    g_assert_false(state.icon_size_found);
    g_assert_cmpint(state.opacity, ==, 82);
    g_assert_true(state.opacity_found);
    g_assert_false(state.show_window_indicator);
    g_assert_true(state.window_indicator_found);
    g_assert_true(state.items_found);
    g_assert_cmpuint(state.item_count, ==, 1);
    g_assert_cmpstr(state.items[0], ==, "app:firefox.desktop");
    g_assert_nonnull(state.warnings);
    g_assert_cmpuint(state.warnings->len, ==, 1);

    dock_config_state_clear(&state);
    test_reset_config();
}


static void
test_snapshot_round_trip_all_settings_and_drawers(void)
{
    test_reset_config();

    const gchar *items[] = {
        "app:firefox.desktop",
        "drawer:0",
        "app:org.mate.Terminal.desktop",
        "drawer:1"
    };
    const gchar *utility_apps[] = {
        "org.mate.Terminal.desktop",
        "caja.desktop"
    };
    const gchar *office_apps[] = {
        "libreoffice-writer.desktop"
    };
    DockConfigDrawer drawers[] = {
        {
            .name = (gchar *)"Utilities",
            .applications = (gchar **)utility_apps,
            .application_count = G_N_ELEMENTS(utility_apps)
        },
        {
            .name = (gchar *)"Office",
            .applications = (gchar **)office_apps,
            .application_count = G_N_ELEMENTS(office_apps)
        }
    };

    GError *error = NULL;
    g_assert_true(
        dock_config_save_state(
            items,
            G_N_ELEMENTS(items),
            drawers,
            G_N_ELEMENTS(drawers),
            TRUE,    /* right side */
            2,       /* top */
            1,       /* monitor index */
            "Dell Inc.",
            "U2720Q",
            56,      /* icon size */
            85,      /* opacity */
            FALSE,   /* running-window indicator */
            FALSE,   /* hide/reveal handle */
            TRUE,    /* shortcut-only */
            TRUE,    /* minimized-window icons */
            FALSE,   /* minimized title labels */
            TRUE,    /* minimized group Drawer */
            FALSE,   /* all workspaces */
            TRUE,    /* bitmap backgrounds */
            1,       /* extend image layout */
            "/tmp/crepido-snapshot-background.png",
            &error));
    g_assert_no_error(error);

    DockConfigState state = {0};
    g_assert_true(dock_config_load_state(&state, &error));
    g_assert_no_error(error);
    g_assert_nonnull(state.warnings);
    g_assert_cmpuint(state.warnings->len, ==, 0);

    g_assert_true(state.side_found);
    g_assert_true(state.on_right_side);
    g_assert_true(state.position_found);
    g_assert_cmpint(state.position_mode, ==, 2);
    g_assert_true(state.monitor_found);
    g_assert_cmpint(state.monitor_index, ==, 1);
    g_assert_true(state.monitor_manufacturer_found);
    g_assert_cmpstr(state.monitor_manufacturer, ==, "Dell Inc.");
    g_assert_true(state.monitor_model_found);
    g_assert_cmpstr(state.monitor_model, ==, "U2720Q");
    g_assert_true(state.icon_size_found);
    g_assert_cmpint(state.icon_size, ==, 56);
    g_assert_true(state.opacity_found);
    g_assert_cmpint(state.opacity, ==, 85);

    g_assert_true(state.window_indicator_found);
    g_assert_false(state.show_window_indicator);
    g_assert_true(state.show_hide_handle_found);
    g_assert_false(state.show_hide_handle);
    g_assert_true(state.shortcut_only_found);
    g_assert_true(state.shortcut_only);
    g_assert_true(state.minimized_window_icons_found);
    g_assert_true(state.minimized_window_icons);
    g_assert_true(state.minimized_title_labels_found);
    g_assert_false(state.minimized_title_labels);
    g_assert_true(state.minimized_group_drawer_found);
    g_assert_true(state.minimized_group_drawer);
    g_assert_true(state.minimized_all_workspaces_found);
    g_assert_false(state.minimized_all_workspaces);

    g_assert_true(state.bitmap_background_enabled_found);
    g_assert_true(state.bitmap_background_enabled);
    g_assert_true(state.bitmap_background_mode_found);
    g_assert_cmpint(state.bitmap_background_mode, ==, 1);
    g_assert_true(state.bitmap_background_path_found);
    g_assert_cmpstr(
        state.bitmap_background_path,
        ==,
        "/tmp/crepido-snapshot-background.png");

    g_assert_true(state.items_found);
    g_assert_cmpuint(state.item_count, ==, G_N_ELEMENTS(items));
    for (gsize i = 0; i < G_N_ELEMENTS(items); i++)
        g_assert_cmpstr(state.items[i], ==, items[i]);

    g_assert_cmpuint(state.drawer_count, ==, 2);
    g_assert_nonnull(state.drawers);
    g_assert_cmpstr(state.drawers[0].name, ==, "Utilities");
    g_assert_cmpuint(
        state.drawers[0].application_count,
        ==,
        G_N_ELEMENTS(utility_apps));
    for (gsize i = 0; i < G_N_ELEMENTS(utility_apps); i++)
        g_assert_cmpstr(
            state.drawers[0].applications[i],
            ==,
            utility_apps[i]);

    g_assert_cmpstr(state.drawers[1].name, ==, "Office");
    g_assert_cmpuint(
        state.drawers[1].application_count,
        ==,
        G_N_ELEMENTS(office_apps));
    g_assert_cmpstr(
        state.drawers[1].applications[0],
        ==,
        office_apps[0]);

    dock_config_state_clear(&state);
    test_reset_config();
}

static void
test_snapshot_legacy_applications(void)
{
    test_reset_config();
    test_write_config(
        "[Dock]\n"
        "Applications=firefox.desktop;mate-terminal.desktop;\n");

    DockConfigState state = {0};
    GError *error = NULL;

    g_assert_true(dock_config_load_state(&state, &error));
    g_assert_no_error(error);
    g_assert_true(state.items_found);
    g_assert_cmpuint(state.item_count, ==, 2);
    g_assert_cmpstr(state.items[0], ==, "app:firefox.desktop");
    g_assert_cmpstr(state.items[1], ==, "app:mate-terminal.desktop");
    g_assert_cmpuint(state.warnings->len, ==, 0);

    dock_config_state_clear(&state);
    test_reset_config();
}

int
main(int argc, char **argv)
{
    GError *error = NULL;

    test_config_home =
        g_dir_make_tmp("crepido-config-tests-XXXXXX", &error);

    if (!test_config_home) {
        g_printerr(
            "Unable to create test configuration directory: %s\n",
            error ? error->message : "unknown error");
        g_clear_error(&error);
        return 1;
    }

    /* Must be set before the first call to g_get_user_config_dir(). */
    g_setenv("XDG_CONFIG_HOME", test_config_home, TRUE);

    g_test_init(&argc, &argv, NULL);

    g_test_add_func(
        "/config/missing-is-not-error",
        test_missing_config_is_not_an_error);
    g_test_add_func(
        "/config/malformed-reports-error",
        test_malformed_config_reports_error);
    g_test_add_func(
        "/config/legacy-applications",
        test_legacy_applications_are_migrated_in_memory);
    g_test_add_func(
        "/config/full-state-round-trip",
        test_full_state_round_trip);
    g_test_add_func(
        "/config/scalar-defaults-and-validation",
        test_scalar_loader_defaults_and_validation);
    g_test_add_func(
        "/config/snapshot-missing-defaults",
        test_snapshot_missing_config_defaults);
    g_test_add_func(
        "/config/snapshot-malformed-setting-isolated",
        test_snapshot_keeps_valid_settings_when_one_is_malformed);
    g_test_add_func(
        "/config/snapshot-round-trip-all-settings-drawers",
        test_snapshot_round_trip_all_settings_and_drawers);
    g_test_add_func(
        "/config/snapshot-legacy-applications",
        test_snapshot_legacy_applications);

    gint result = g_test_run();

    test_reset_config();
    g_rmdir(test_config_home);
    g_free(test_config_home);

    return result;
}
