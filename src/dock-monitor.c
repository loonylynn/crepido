/*
 * Pure helpers for monitor identity selection.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "dock-monitor.h"

static gboolean
dock_monitor_identity_matches(
    const gchar *manufacturer,
    const gchar *model,
    const DockMonitorIdentity *monitor)
{
    if (!monitor)
        return FALSE;

    gboolean have_manufacturer =
        manufacturer && *manufacturer;
    gboolean have_model =
        model && *model;

    if (!have_manufacturer && !have_model)
        return FALSE;

    return (!have_manufacturer ||
            g_strcmp0(
                manufacturer,
                monitor->manufacturer) == 0) &&
           (!have_model ||
            g_strcmp0(
                model,
                monitor->model) == 0);
}

gint
dock_monitor_find_identity_index(
    const gchar *manufacturer,
    const gchar *model,
    const DockMonitorIdentity *monitors,
    gsize monitor_count,
    gint preferred_index)
{
    gboolean have_identity =
        (manufacturer && *manufacturer) ||
        (model && *model);

    if (!have_identity || !monitors)
        return preferred_index;

    /*
     * If the saved slot still holds the requested identity, keep it. Two
     * physically distinct monitors can report identical make/model strings.
     */
    if (preferred_index >= 0 &&
        (gsize)preferred_index < monitor_count &&
        dock_monitor_identity_matches(
            manufacturer,
            model,
            &monitors[preferred_index]))
        return preferred_index;

    for (gsize i = 0; i < monitor_count; i++) {
        if (dock_monitor_identity_matches(
                manufacturer,
                model,
                &monitors[i]))
            return (gint)i;
    }

    return preferred_index;
}
