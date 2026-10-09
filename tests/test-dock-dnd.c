/*
 * Regression tests for Dock/Drawer drag destination resolution.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>

#include "dock-dnd.h"

static void
test_drawer_target_precedes_overlapping_dock(void)
{
    DockDndDropTarget target =
        dock_dnd_resolve_drop_target(
            TRUE,
            TRUE,
            2,
            4);

    g_assert_cmpint(target.kind, ==, DOCK_DND_DROP_DRAWER);
    g_assert_cmpint(target.slot, ==, 2);
}

static void
test_drawer_target_uses_its_insertion_slot(void)
{
    DockDndDropTarget target =
        dock_dnd_resolve_drop_target(
            FALSE,
            TRUE,
            5,
            -1);

    g_assert_cmpint(target.kind, ==, DOCK_DND_DROP_DRAWER);
    g_assert_cmpint(target.slot, ==, 5);
}

static void
test_dock_target_is_used_without_drawer_hit(void)
{
    DockDndDropTarget target =
        dock_dnd_resolve_drop_target(
            TRUE,
            FALSE,
            -1,
            3);

    g_assert_cmpint(target.kind, ==, DOCK_DND_DROP_DOCK);
    g_assert_cmpint(target.slot, ==, 3);
}

static void
test_outside_targets_resolve_to_none(void)
{
    DockDndDropTarget target =
        dock_dnd_resolve_drop_target(
            FALSE,
            FALSE,
            -1,
            -1);

    g_assert_cmpint(target.kind, ==, DOCK_DND_DROP_NONE);
    g_assert_cmpint(target.slot, ==, -1);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func(
        "/dock-dnd/drawer-precedes-overlapping-dock",
        test_drawer_target_precedes_overlapping_dock);
    g_test_add_func(
        "/dock-dnd/drawer-insertion-slot",
        test_drawer_target_uses_its_insertion_slot);
    g_test_add_func(
        "/dock-dnd/dock-fallback",
        test_dock_target_is_used_without_drawer_hit);
    g_test_add_func(
        "/dock-dnd/no-target",
        test_outside_targets_resolve_to_none);

    return g_test_run();
}
