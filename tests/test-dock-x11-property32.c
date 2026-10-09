/*
 * Regression tests for Xlib format-32 property conversion.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>

#include "dock-x11-property32.h"

static void
test_reads_low_32_bits_of_native_long(void)
{
    unsigned long items[] = {
        (G_MAXULONG & ~((unsigned long)G_MAXUINT32)) |
        (unsigned long)0x89ABCDEFu
    };

    guint32 value = 0;

    g_assert_true(
        dock_x11_property32_read(
            items,
            G_N_ELEMENTS(items),
            0,
            &value));
    g_assert_cmphex(value, ==, 0x89ABCDEFu);
}

static void
test_reads_selected_item(void)
{
    const unsigned long items[] = {
        0x00000001UL,
        0x12345678UL,
        0xFFFFFFFFUL
    };
    guint32 value = 0;

    g_assert_true(
        dock_x11_property32_read(
            items,
            G_N_ELEMENTS(items),
            1,
            &value));
    g_assert_cmphex(value, ==, 0x12345678u);

    g_assert_true(
        dock_x11_property32_read(
            items,
            G_N_ELEMENTS(items),
            2,
            &value));
    g_assert_cmphex(value, ==, 0xFFFFFFFFu);
}

static void
test_rejects_out_of_range_index(void)
{
    const unsigned long items[] = { 1UL };
    guint32 value = 0xA5A5A5A5u;

    g_assert_false(
        dock_x11_property32_read(
            items,
            G_N_ELEMENTS(items),
            1,
            &value));
    g_assert_cmphex(value, ==, 0xA5A5A5A5u);
}

static void
test_rejects_null_arguments(void)
{
    const unsigned long items[] = { 1UL };
    guint32 value = 0;

    g_assert_false(
        dock_x11_property32_read(
            NULL,
            1,
            0,
            &value));
    g_assert_false(
        dock_x11_property32_read(
            items,
            1,
            0,
            NULL));
    g_assert_false(
        dock_x11_property32_read(
            items,
            0,
            0,
            &value));
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func(
        "/x11-property32/low-32-bits",
        test_reads_low_32_bits_of_native_long);
    g_test_add_func(
        "/x11-property32/selected-item",
        test_reads_selected_item);
    g_test_add_func(
        "/x11-property32/out-of-range",
        test_rejects_out_of_range_index);
    g_test_add_func(
        "/x11-property32/null-arguments",
        test_rejects_null_arguments);

    return g_test_run();
}
