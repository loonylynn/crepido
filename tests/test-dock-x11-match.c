/*
 * Regression tests for X11 application-window identity scoring.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>

#include "dock-x11-match.h"

static void
test_startup_wm_class_matches_res_name(void)
{
    const DockX11MatchCandidate candidate = {
        .wm_class_name = "firefox",
        .wm_class_class = "Navigator",
        .process_basename = "firefox"
    };

    g_assert_cmpint(
        dock_x11_match_score(
            &candidate,
            "FIREFOX",
            "firefox"),
        ==,
        200);
}

static void
test_startup_wm_class_matches_res_class(void)
{
    const DockX11MatchCandidate candidate = {
        .wm_class_name = "navigator",
        .wm_class_class = "Firefox",
        .process_basename = "firefox"
    };

    g_assert_cmpint(
        dock_x11_match_score(
            &candidate,
            "FIREFOX",
            "unrelated-executable"),
        ==,
        200);
}

static void
test_exec_basename_matches_wm_class(void)
{
    const DockX11MatchCandidate candidate = {
        .wm_class_name = "Mate-Terminal",
        .wm_class_class = "Terminal",
        .process_basename = "some-wrapper"
    };

    g_assert_cmpint(
        dock_x11_match_score(
            &candidate,
            NULL,
            "mate-terminal"),
        ==,
        DOCK_X11_MATCH_MIN_SCORE);
}

static void
test_process_executable_is_strongest_fallback(void)
{
    const DockX11MatchCandidate candidate = {
        .wm_class_name = "browser-window",
        .wm_class_class = "BrowserWindow",
        .process_basename = "firefox"
    };

    g_assert_cmpint(
        dock_x11_match_score(
            &candidate,
            NULL,
            "FIREFOX"),
        ==,
        220);
}

static void
test_process_match_raises_exec_class_score(void)
{
    const DockX11MatchCandidate candidate = {
        .wm_class_name = "Firefox",
        .wm_class_class = "Navigator",
        .process_basename = "firefox"
    };

    g_assert_cmpint(
        dock_x11_match_score(
            &candidate,
            NULL,
            "firefox"),
        ==,
        220);
}

static void
test_startup_class_keeps_existing_precedence(void)
{
    const DockX11MatchCandidate candidate = {
        .wm_class_name = "Firefox",
        .wm_class_class = "Navigator",
        .process_basename = "firefox"
    };

    /*
     * Current policy returns the StartupWMClass score immediately, even
     * when the executable basename would otherwise score higher.
     */
    g_assert_cmpint(
        dock_x11_match_score(
            &candidate,
            "navigator",
            "firefox"),
        ==,
        200);
}

static void
test_unrelated_window_is_not_matched(void)
{
    const DockX11MatchCandidate candidate = {
        .wm_class_name = "desktop",
        .wm_class_class = "Desktop",
        .process_basename = "xfdesktop"
    };

    g_assert_cmpint(
        dock_x11_match_score(
            &candidate,
            "firefox",
            "firefox"),
        ==,
        0);
}

static void
test_dock_window_is_excluded(void)
{
    const DockX11MatchCandidate candidate = {
        .wm_class_name = "firefox",
        .wm_class_class = "Firefox",
        .process_basename = "firefox",
        .dock_window = TRUE
    };

    g_assert_cmpint(
        dock_x11_match_score(
            &candidate,
            "firefox",
            "firefox"),
        ==,
        0);
}

static void
test_desktop_window_is_excluded(void)
{
    const DockX11MatchCandidate candidate = {
        .wm_class_name = "Caja",
        .wm_class_class = "Caja",
        .process_basename = "caja",
        .desktop_window = TRUE
    };

    g_assert_cmpint(
        dock_x11_match_score(
            &candidate,
            "caja",
            "caja"),
        ==,
        0);
}

static void
test_empty_or_missing_identity_is_not_matched(void)
{
    const DockX11MatchCandidate candidate = { 0 };

    g_assert_cmpint(
        dock_x11_match_score(
            &candidate,
            "",
            ""),
        ==,
        0);
    g_assert_cmpint(
        dock_x11_match_score(
            NULL,
            "firefox",
            "firefox"),
        ==,
        0);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func(
        "/x11-match/startup-wm-class-res-name",
        test_startup_wm_class_matches_res_name);
    g_test_add_func(
        "/x11-match/startup-wm-class-res-class",
        test_startup_wm_class_matches_res_class);
    g_test_add_func(
        "/x11-match/exec-basename-wm-class",
        test_exec_basename_matches_wm_class);
    g_test_add_func(
        "/x11-match/process-executable-strongest-fallback",
        test_process_executable_is_strongest_fallback);
    g_test_add_func(
        "/x11-match/process-raises-exec-class-score",
        test_process_match_raises_exec_class_score);
    g_test_add_func(
        "/x11-match/startup-class-precedence",
        test_startup_class_keeps_existing_precedence);
    g_test_add_func(
        "/x11-match/unrelated-window",
        test_unrelated_window_is_not_matched);
    g_test_add_func(
        "/x11-match/dock-excluded",
        test_dock_window_is_excluded);
    g_test_add_func(
        "/x11-match/desktop-excluded",
        test_desktop_window_is_excluded);
    g_test_add_func(
        "/x11-match/empty-or-missing-identity",
        test_empty_or_missing_identity_is_not_matched);

    return g_test_run();
}
