/*
 * Display-independent application-window identity scoring.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "dock-x11-match.h"

static gboolean
same_name_ci(
    const gchar *a,
    const gchar *b)
{
    if (!a || !b || !*a || !*b)
        return FALSE;

    return g_ascii_strcasecmp(a, b) == 0;
}

gint
dock_x11_match_score(
    const DockX11MatchCandidate *candidate,
    const gchar *startup_wm_class,
    const gchar *exec_basename)
{
    if (!candidate ||
        candidate->dock_window ||
        candidate->desktop_window)
        return 0;

    /*
     * StartupWMClass is the strongest configured application identity.
     * Preserve the original precedence: an exact startup-class match
     * returns before process identity is considered.
     */
    if (startup_wm_class &&
        (same_name_ci(
             candidate->wm_class_name,
             startup_wm_class) ||
         same_name_ci(
             candidate->wm_class_class,
             startup_wm_class)))
        return 200;

    gint score = 0;

    if (exec_basename &&
        (same_name_ci(
             candidate->wm_class_name,
             exec_basename) ||
         same_name_ci(
             candidate->wm_class_class,
             exec_basename)))
        score = 100;

    if (exec_basename &&
        same_name_ci(
            candidate->process_basename,
            exec_basename))
        score = MAX(score, 220);

    return score;
}
