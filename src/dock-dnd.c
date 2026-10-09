/*
 * Internal helpers for resolving Dock/Drawer drag destinations.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "dock-dnd.h"

DockDndDropTarget
dock_dnd_resolve_drop_target(
    gboolean over_dock,
    gboolean has_drawer_target,
    gint drawer_slot,
    gint dock_slot)
{
    DockDndDropTarget target = {
        .kind = DOCK_DND_DROP_NONE,
        .slot = -1
    };

    /*
     * Drawer tiles occupy space inside the Dock window. A valid Drawer hit
     * must therefore take precedence over the more general Dock hit test.
     */
    if (has_drawer_target) {
        target.kind = DOCK_DND_DROP_DRAWER;
        target.slot = drawer_slot;
    } else if (over_dock) {
        target.kind = DOCK_DND_DROP_DOCK;
        target.slot = dock_slot;
    }

    return target;
}
