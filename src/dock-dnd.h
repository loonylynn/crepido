/*
 * Internal helpers for resolving Dock/Drawer drag destinations.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CREPIDO_DOCK_DND_H
#define CREPIDO_DOCK_DND_H

#include <glib.h>

typedef enum {
    DOCK_DND_DROP_NONE,
    DOCK_DND_DROP_DOCK,
    DOCK_DND_DROP_DRAWER
} DockDndDropKind;

typedef struct {
    DockDndDropKind kind;
    gint slot;
} DockDndDropTarget;

DockDndDropTarget
dock_dnd_resolve_drop_target(
    gboolean over_dock,
    gboolean has_drawer_target,
    gint drawer_slot,
    gint dock_slot);

#endif
