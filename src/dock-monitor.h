/*
 * Pure helpers for monitor identity selection.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef DOCK_MONITOR_H
#define DOCK_MONITOR_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
    const gchar *manufacturer;
    const gchar *model;
} DockMonitorIdentity;

/*
 * Prefer preferred_index when it still matches the requested identity.
 * Otherwise return the first identity match, or preferred_index if none
 * matches (the caller can then apply its normal index/primary fallback).
 */
gint dock_monitor_find_identity_index(
    const gchar *manufacturer,
    const gchar *model,
    const DockMonitorIdentity *monitors,
    gsize monitor_count,
    gint preferred_index);

G_END_DECLS

#endif
