/*
 * Display-independent application-window identity scoring.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CREPIDO_DOCK_X11_MATCH_H
#define CREPIDO_DOCK_X11_MATCH_H

#include <glib.h>

G_BEGIN_DECLS

enum {
    DOCK_X11_MATCH_MIN_SCORE = 100
};

/*
 * Borrowed identity data for a single X11 client. This module deliberately
 * has no Xlib dependency so matching policy can be tested without a display.
 */
typedef struct {
    const gchar *wm_class_name;
    const gchar *wm_class_class;
    const gchar *process_basename;
    gboolean dock_window;
    gboolean desktop_window;
} DockX11MatchCandidate;

/*
 * Returns 0 for excluded or unrelated windows, 100 for an executable-name
 * WM_CLASS match, 200 for a StartupWMClass match, and 220 for a matching
 * process executable. A StartupWMClass match retains its existing priority.
 */
gint dock_x11_match_score(
    const DockX11MatchCandidate *candidate,
    const gchar *startup_wm_class,
    const gchar *exec_basename);

G_END_DECLS

#endif /* CREPIDO_DOCK_X11_MATCH_H */
