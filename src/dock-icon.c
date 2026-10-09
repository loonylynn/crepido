/*
 * crepido
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "dock-icon.h"


static gboolean
dock_icon_launch_spinner_timeout(
    gpointer user_data)
{
    GtkWidget *button = user_data;

    if (!button ||
        !GTK_IS_WIDGET(button))
        return G_SOURCE_REMOVE;

    g_object_set_data(
        G_OBJECT(button),
        "dock-launch-spinner-timeout",
        GUINT_TO_POINTER(0));

    GtkWidget *spinner =
        g_object_get_data(
            G_OBJECT(button),
            "dock-launch-spinner");

    if (spinner &&
        GTK_IS_SPINNER(spinner)) {
        gtk_spinner_stop(
            GTK_SPINNER(spinner));
        gtk_widget_hide(spinner);
    }

    g_object_unref(button);

    return G_SOURCE_REMOVE;
}

static GtkWidget *
create_icon_button(DockLauncher *launcher, gboolean interactive)
{
    (void)interactive;

    g_return_val_if_fail(launcher != NULL, NULL);

    GtkWidget *button = gtk_button_new();
    gtk_style_context_add_class(
        gtk_widget_get_style_context(button),
        "crepido-bitmap-block");

    /*
     * Keep the slot exactly 64x64 and let the active GTK theme render
     * the native GtkButton. Do not override its colors, borders, padding,
     * or relief metrics.
     */
    gtk_widget_set_size_request(button, 64, 64);
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NORMAL);
    gtk_widget_set_can_focus(button, FALSE);
    gtk_widget_set_focus_on_click(button, FALSE);

    GAppInfo *app_info = dock_launcher_get_app_info(launcher);
    GIcon *app_icon = g_app_info_get_icon(app_info);

    GtkWidget *image = NULL;

    if (app_icon) {
        image = gtk_image_new_from_gicon(
            app_icon, GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(GTK_IMAGE(image), 48);
    } else {
        image = gtk_image_new_from_icon_name(
            "application-x-executable", GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(GTK_IMAGE(image), 48);
    }

    /*
     * Put the icon in an overlay so the X11 window-group count can be
     * displayed directly over its upper-left corner. The badge is a normal
     * themed GtkLabel; we do not supply custom colors or CSS.
     */
    GtkWidget *overlay =
        gtk_overlay_new();

    gtk_container_add(
        GTK_CONTAINER(overlay),
        image);

    /*
     * Draw a one-pixel white shadow behind the count, while leaving the
     * number itself theme-controlled. The shadow is another normal label,
     * offset by one pixel down/right.
     */
    GtkWidget *count_shadow =
        gtk_label_new(NULL);

    gtk_widget_set_halign(
        count_shadow,
        GTK_ALIGN_START);
    gtk_widget_set_valign(
        count_shadow,
        GTK_ALIGN_END);
    gtk_widget_set_margin_start(
        count_shadow,
        3);
    gtk_widget_set_margin_bottom(
        count_shadow,
        0);
    gtk_widget_set_opacity(
        count_shadow,
        1.0);
    gtk_widget_hide(count_shadow);

    gtk_overlay_add_overlay(
        GTK_OVERLAY(overlay),
        count_shadow);

    GtkWidget *launch_spinner =
        gtk_spinner_new();

    gtk_widget_set_halign(
        launch_spinner,
        GTK_ALIGN_END);
    gtk_widget_set_valign(
        launch_spinner,
        GTK_ALIGN_END);
    gtk_widget_set_margin_end(
        launch_spinner,
        3);
    gtk_widget_set_margin_bottom(
        launch_spinner,
        3);
    gtk_widget_set_size_request(
        launch_spinner,
        14,
        14);
    gtk_widget_set_no_show_all(
        launch_spinner,
        TRUE);

    gtk_overlay_add_overlay(
        GTK_OVERLAY(overlay),
        launch_spinner);

    GtkWidget *count_label =
        gtk_label_new(NULL);

    gtk_widget_set_halign(
        count_label,
        GTK_ALIGN_START);
    gtk_widget_set_valign(
        count_label,
        GTK_ALIGN_END);
    gtk_widget_set_margin_start(
        count_label,
        2);
    gtk_widget_set_margin_bottom(
        count_label,
        1);
    gtk_widget_hide(count_label);

    gtk_overlay_add_overlay(
        GTK_OVERLAY(overlay),
        count_label);

    /*
     * Use the overlay as the GtkButton's actual child rather than as the
     * GtkButton image property. This keeps the image as a normal visible
     * child while still letting the themed button draw its frame.
     */
    gtk_container_add(
        GTK_CONTAINER(button),
        overlay);

    gtk_widget_show(image);
    gtk_widget_show(overlay);

    g_object_set_data(
        G_OBJECT(button),
        "dock-icon-image",
        image);

    gtk_widget_set_tooltip_text(
        button,
        dock_launcher_get_name(launcher));

    g_object_set_data(
        G_OBJECT(button),
        "dock-instance-count-label",
        count_label);

    g_object_set_data(
        G_OBJECT(button),
        "dock-instance-count-shadow",
        count_shadow);

    g_object_set_data(
        G_OBJECT(button),
        "dock-launch-spinner",
        launch_spinner);

    /*
     * The Dock model owns the launcher. The button keeps only a borrowed
     * reference so destroying the widget cannot free the model's object.
     */
    g_object_set_data(
        G_OBJECT(button),
        "dock-launcher",
        launcher);

    return button;
}

GtkWidget *
dock_icon_button_new(DockLauncher *launcher)
{
    return create_icon_button(launcher, TRUE);
}

GtkWidget *
dock_icon_button_new_drag_preview(DockLauncher *launcher)
{
    return create_icon_button(launcher, FALSE);
}

void
dock_icon_button_set_launcher(
    GtkWidget *button,
    DockLauncher *launcher)
{
    g_return_if_fail(
        GTK_IS_WIDGET(button));
    g_return_if_fail(
        launcher != NULL);

    GtkWidget *image =
        g_object_get_data(
            G_OBJECT(button),
            "dock-icon-image");

    if (image && GTK_IS_IMAGE(image)) {
        GAppInfo *app_info =
            dock_launcher_get_app_info(launcher);

        GIcon *icon =
            g_app_info_get_icon(app_info);

        if (icon) {
            gtk_image_set_from_gicon(
                GTK_IMAGE(image),
                icon,
                GTK_ICON_SIZE_DIALOG);
        } else {
            gtk_image_set_from_icon_name(
                GTK_IMAGE(image),
                "application-x-executable",
                GTK_ICON_SIZE_DIALOG);
        }
    }

    gtk_widget_set_tooltip_text(
        button,
        dock_launcher_get_name(launcher));

    g_object_set_data(
        G_OBJECT(button),
        "dock-launcher",
        launcher);
}

void
dock_icon_button_set_icon_size(
    GtkWidget *button,
    gint icon_size)
{
    g_return_if_fail(
        GTK_IS_WIDGET(button));

    GtkWidget *image =
        g_object_get_data(
            G_OBJECT(button),
            "dock-icon-image");

    if (!image ||
        !GTK_IS_IMAGE(image))
        return;

    icon_size = CLAMP(
        icon_size,
        16,
        64);

    gtk_image_set_pixel_size(
        GTK_IMAGE(image),
        icon_size);
}


void
dock_icon_button_set_instance_count(
    GtkWidget *button,
    guint count)
{
    g_return_if_fail(
        GTK_IS_WIDGET(button));

    GtkWidget *label =
        g_object_get_data(
            G_OBJECT(button),
            "dock-instance-count-label");

    GtkWidget *shadow =
        g_object_get_data(
            G_OBJECT(button),
            "dock-instance-count-shadow");

    if (!label ||
        !GTK_IS_LABEL(label) ||
        !shadow ||
        !GTK_IS_LABEL(shadow))
        return;

    if (count > 0) {
        gchar *text =
            count > 1 ?
            g_strdup_printf("%u", count) :
            g_strdup("•");

        gtk_label_set_text(
            GTK_LABEL(label),
            text);

        gchar *shadow_markup =
            g_markup_printf_escaped(
                "<span foreground=\"#ffffff\">%s</span>",
                text);

        gtk_label_set_markup(
            GTK_LABEL(shadow),
            shadow_markup);

        g_free(shadow_markup);
        g_free(text);

        gtk_widget_show(shadow);
        gtk_widget_show(label);
    } else {
        gtk_widget_hide(shadow);
        gtk_widget_hide(label);
    }
}

void
dock_icon_button_set_launching(
    GtkWidget *button,
    gboolean launching)
{
    g_return_if_fail(
        GTK_IS_WIDGET(button));

    GtkWidget *spinner =
        g_object_get_data(
            G_OBJECT(button),
            "dock-launch-spinner");

    if (!spinner ||
        !GTK_IS_SPINNER(spinner))
        return;

    guint timeout_id =
        GPOINTER_TO_UINT(
            g_object_get_data(
                G_OBJECT(button),
                "dock-launch-spinner-timeout"));

    if (!launching) {
        if (timeout_id) {
            g_source_remove(timeout_id);

            /*
             * The timeout owns a reference to the button while it is
             * pending. Since removing the source prevents its callback
             * from running, release that reference here.
             */
            g_object_unref(button);

            g_object_set_data(
                G_OBJECT(button),
                "dock-launch-spinner-timeout",
                GUINT_TO_POINTER(0));
        }

        gtk_spinner_stop(
            GTK_SPINNER(spinner));
        gtk_widget_hide(spinner);
        return;
    }

    if (!timeout_id) {
        g_object_ref(button);

        timeout_id =
            g_timeout_add_seconds(
                5,
                dock_icon_launch_spinner_timeout,
                button);

        g_object_set_data(
            G_OBJECT(button),
            "dock-launch-spinner-timeout",
            GUINT_TO_POINTER(timeout_id));
    }

    gtk_widget_show(spinner);
    gtk_spinner_start(
        GTK_SPINNER(spinner));
}

