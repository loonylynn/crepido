/*
 * Helpers for reading Xlib format-32 property values.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "dock-x11-property32.h"

gboolean
dock_x11_property32_read(
    const unsigned long *items,
    gsize item_count,
    gsize index,
    guint32 *value_out)
{
    if (!items ||
        !value_out ||
        index >= item_count)
        return FALSE;

    /*
     * Xlib uses unsigned long-sized slots for format 32, including on LP64.
     * Conversion to guint32 intentionally discards any padding above bit 31.
     */
    *value_out = (guint32)items[index];

    return TRUE;
}
