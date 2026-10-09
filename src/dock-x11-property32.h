/*
 * Helpers for reading Xlib format-32 property values.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CREPIDO_DOCK_X11_PROPERTY32_H
#define CREPIDO_DOCK_X11_PROPERTY32_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * XGetWindowProperty returns format-32 data in native unsigned long slots.
 * On LP64 systems those slots are 64 bits wide, but the property value is
 * still only 32 bits. Read one slot and explicitly normalize it to guint32.
 */
gboolean dock_x11_property32_read(
    const unsigned long *items,
    gsize item_count,
    gsize index,
    guint32 *value_out);

G_END_DECLS

#endif /* CREPIDO_DOCK_X11_PROPERTY32_H */
