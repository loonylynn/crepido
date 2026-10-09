/*
 * Crepido monitor identity-selection regression tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>

#include "dock-monitor.h"

static void
test_duplicate_identity_preserves_preferred_index(void)
{
    const DockMonitorIdentity monitors[] = {
        { "Dell Inc.", "U2720Q" },
        { "Dell Inc.", "U2720Q" },
        { "ACME", "Panel" }
    };

    g_assert_cmpint(
        dock_monitor_find_identity_index(
            "Dell Inc.", "U2720Q",
            monitors, G_N_ELEMENTS(monitors), 1),
        ==,
        1);
}

static void
test_missing_preferred_identity_searches_for_match(void)
{
    const DockMonitorIdentity monitors[] = {
        { "Dell Inc.", "U2720Q" },
        { "ACME", "Panel" },
        { "Dell Inc.", "U2720Q" }
    };

    g_assert_cmpint(
        dock_monitor_find_identity_index(
            "Dell Inc.", "U2720Q",
            monitors, G_N_ELEMENTS(monitors), 1),
        ==,
        0);
}

static void
test_changed_order_uses_new_identity_index(void)
{
    const DockMonitorIdentity monitors[] = {
        { "ACME", "Panel" },
        { "Dell Inc.", "U2720Q" }
    };

    g_assert_cmpint(
        dock_monitor_find_identity_index(
            "Dell Inc.", "U2720Q",
            monitors, G_N_ELEMENTS(monitors), 0),
        ==,
        1);
}

static void
test_partial_identity_is_supported(void)
{
    const DockMonitorIdentity monitors[] = {
        { "Dell Inc.", "U2720Q" },
        { "ACME", "Panel" }
    };

    g_assert_cmpint(
        dock_monitor_find_identity_index(
            NULL, "Panel",
            monitors, G_N_ELEMENTS(monitors), 0),
        ==,
        1);
}

static void
test_unknown_identity_keeps_fallback_index(void)
{
    const DockMonitorIdentity monitors[] = {
        { "Dell Inc.", "U2720Q" },
        { "ACME", "Panel" }
    };

    g_assert_cmpint(
        dock_monitor_find_identity_index(
            "Unknown", "Display",
            monitors, G_N_ELEMENTS(monitors), 1),
        ==,
        1);
}

static void
test_absent_identity_keeps_fallback_index(void)
{
    const DockMonitorIdentity monitors[] = {
        { "Dell Inc.", "U2720Q" },
        { "ACME", "Panel" }
    };

    g_assert_cmpint(
        dock_monitor_find_identity_index(
            NULL, NULL,
            monitors, G_N_ELEMENTS(monitors), 7),
        ==,
        7);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func(
        "/dock-monitor/duplicate-preserves-preferred",
        test_duplicate_identity_preserves_preferred_index);
    g_test_add_func(
        "/dock-monitor/missing-preferred-searches",
        test_missing_preferred_identity_searches_for_match);
    g_test_add_func(
        "/dock-monitor/changed-order-searches",
        test_changed_order_uses_new_identity_index);
    g_test_add_func(
        "/dock-monitor/partial-identity",
        test_partial_identity_is_supported);
    g_test_add_func(
        "/dock-monitor/unknown-identity-fallback",
        test_unknown_identity_keeps_fallback_index);
    g_test_add_func(
        "/dock-monitor/absent-identity-fallback",
        test_absent_identity_keeps_fallback_index);

    return g_test_run();
}
