/*
 * crepido
 *
 * Stage 6: persistent Dock with GTK context menus for adding/removing apps.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "dock.h"

#include <gio/gdesktopappinfo.h>
#include <X11/Xlib.h>
#include <gdk/gdkx.h>
#include <math.h>

#include "dock-icon.h"
#include "dock-launcher.h"
#include "dock-config.h"
#include "dock-dnd.h"
#include "dock-x11.h"

static gint dock_tile_size = 64;

static gint
dock_get_tile_size(void)
{
    return dock_tile_size;
}

#define DOCK_TILE_SIZE dock_tile_size

#define DOCK_HIDE_ANIMATION_DURATION_US 180000


static GtkWidget *
dock_menu_item_new(
    const gchar *label,
    const gchar *icon_name)
{
    GtkWidget *item =
        gtk_menu_item_new();

    GtkWidget *box =
        gtk_box_new(
            GTK_ORIENTATION_HORIZONTAL,
            6);

    GtkWidget *icon_box =
        gtk_box_new(
            GTK_ORIENTATION_HORIZONTAL,
            0);

    GtkWidget *image =
        gtk_image_new_from_icon_name(
            icon_name,
            GTK_ICON_SIZE_MENU);

    GtkWidget *text =
        gtk_label_new(label);

    gtk_widget_set_size_request(
        icon_box,
        16,
        -1);

    gtk_widget_set_halign(
        text,
        GTK_ALIGN_START);

    gtk_widget_set_hexpand(
        text,
        TRUE);

    gtk_box_pack_start(
        GTK_BOX(icon_box),
        image,
        FALSE,
        FALSE,
        0);

    gtk_box_pack_start(
        GTK_BOX(box),
        icon_box,
        FALSE,
        FALSE,
        0);

    gtk_box_pack_start(
        GTK_BOX(box),
        text,
        TRUE,
        TRUE,
        0);

    gtk_container_add(
        GTK_CONTAINER(item),
        box);

    return item;
}

/* Higher rate keeps the grabbed icon close to the pointer. */
#define DRAG_FOLLOW_RATE 72.0

/* Neighbors move slightly more softly, producing the taskbar slide effect. */
#define DRAG_NEIGHBOR_RATE 28.0
#define DRAWER_ANIMATION_USEC 280000.0
#define DRAWER_HOVER_CLOSE_DELAY_MS 900
#define DRAWER_DRAG_HOVER_OPEN_DELAY_MS 120

typedef struct _Dock Dock;
typedef struct _DockDrawer DockDrawer;

typedef struct {
    gchar *desktop_id;
    DockLauncher *launcher;
    GtkWidget *button;
    gdouble visual_x;
    gdouble target_x;
    gint applied_x;
} DockDrawerEntry;

struct _DockDrawer {
    Dock *dock;
    gchar *name;
    GtkWidget *button;
    GtkWidget *popup;
    GPtrArray *entries;
    guint outside_poll_id;
    guint animation_tick_id;
    gint64 animation_start_us;
    gdouble animation_progress;
    gboolean animation_closing;
    guint hover_open_id;
    guint hover_close_id;

    gboolean drag_press_active;
    gboolean dragging;
    gboolean dropping;
    GtkWidget *drag_button;
    DockDrawerEntry *drag_entry;
    gdouble press_root_x;
    gdouble press_root_y;
    gdouble drag_target_x;

    GtkWidget *drag_proxy;
    gdouble drag_proxy_x;
    gdouble drag_proxy_y;
    gdouble drag_proxy_target_x;
    gdouble drag_proxy_target_y;
    gboolean drag_proxy_visible;

    gint source_index;
    gint target_index;
    DockDrawer *drag_target_drawer;
    gint drag_target_slot;
    gint64 drag_last_frame_us;
    guint drag_tick_id;
};

typedef struct {
    gchar *desktop_id;
    DockLauncher *launcher;
    GtkWidget *button;
    gboolean is_drawer;
    DockDrawer *drawer;

    gdouble visual_y;
    gdouble target_y;

    gint applied_y;
} DockItem;

struct _Dock {
    GPtrArray *items;
    GtkWidget *box;
    GtkWidget *hide_handle_button;
    GtkWidget *hide_handle_image;
    DockX11 *x11;
    gboolean hidden;
    gboolean hide_animating;
    guint hide_animation_tick_id;
    gint hide_animation_start_x;
    gint hide_animation_start_y;
    gint hide_animation_target_x;
    gint hide_animation_target_y;
    gint64 hide_animation_start_us;
    gboolean on_right_side;
    DockPositionMode position_mode;
    gint monitor_index;
    GdkDisplay *monitor_display;
    GdkScreen *monitor_screen;
    GdkMonitor *monitor_object;
    gchar *monitor_manufacturer;
    gchar *monitor_model;
    gulong monitor_added_handler;
    gulong monitor_removed_handler;
    gulong monitors_changed_handler;
    gulong screen_size_changed_handler;
    guint monitor_refresh_id;
    gint icon_size;
    gint opacity_percent;
    gboolean bitmap_background_enabled;
    gchar *bitmap_background_path;
    DockBitmapBackgroundMode bitmap_background_mode;
    GtkCssProvider *bitmap_background_provider;
    gboolean show_window_indicator;
    gboolean show_hide_handle;
    gboolean shortcut_only;
    gboolean minimized_window_icons;
    gboolean minimized_title_labels;
    gboolean minimized_group_drawer;
    gboolean minimized_all_workspaces;

    gboolean dragging;
    gboolean dropping;

    DockItem *drag_item;
    GtkWidget *drag_button;

    gint source_index;
    gint target_index;

    gboolean press_active;
    GtkWidget *press_button;
    gdouble press_root_x;
    gdouble press_root_y;

    gdouble drag_target_y;

    GtkWidget *drag_proxy;
    gdouble drag_proxy_x;
    gdouble drag_proxy_y;
    gdouble drag_proxy_target_x;
    gdouble drag_proxy_target_y;
    gboolean drag_proxy_visible;

    DockDrawer *drag_hover_drawer;
    DockDrawer *drag_drop_drawer;
    gint drag_drop_slot;
    gboolean drag_drop_on_tile;

    gboolean drawer_drop_on_dock;
    gint drawer_drop_dock_slot;

    guint64 press_active_window;
    gint press_group_popup_state;
    gint64 last_frame_us;
    guint tick_id;
};


/*
 * The reveal button is a half-height slot at the bottom of the vertical Dock.
 * The window slides upward until that short slot reaches the top screen edge.
 */
static gint
dock_get_handle_height(const Dock *dock)
{
    if (!dock || !dock->show_hide_handle)
        return 0;

    if (dock->hide_handle_button) {
        GtkAllocation allocation;
        gtk_widget_get_allocation(
            dock->hide_handle_button,
            &allocation);

        if (allocation.height > 1)
            return allocation.height;
    }

    return MAX(dock_get_tile_size() / 2, 1);
}

static gint
dock_get_window_width(const Dock *dock)
{
    (void)dock;
    return dock_get_tile_size();
}

static gint
dock_get_window_height(const Dock *dock)
{
    gint item_count =
        dock && dock->items ?
            (gint)dock->items->len :
            0;

    if (dock && dock->show_hide_handle)
        return item_count * DOCK_TILE_SIZE +
            MAX(DOCK_TILE_SIZE / 2, 1);

    return DOCK_TILE_SIZE * MAX(item_count, 1);
}

static gint
dock_get_items_x(const Dock *dock)
{
    (void)dock;
    return 0;
}

static gint
dock_get_handle_y(const Dock *dock)
{
    if (!dock || !dock->items)
        return 0;

    return (gint)dock->items->len * DOCK_TILE_SIZE;
}

static gboolean dock_drag_tick(
    GtkWidget *widget,
    GdkFrameClock *frame_clock,
    gpointer user_data);

static void
dock_ensure_tick(
    Dock *dock);

static void dock_drag_proxy_destroy(
    Dock *dock);

static void dock_drag_proxy_update(
    Dock *dock,
    gboolean visible,
    gdouble root_x,
    gdouble root_y);

static void dock_drawer_clear_drop_states(
    Dock *dock);

static gboolean dock_insertion_indicator_draw(
    GtkWidget *widget,
    cairo_t *cr,
    gpointer user_data);

static void dock_update_drawer_drop_indicator(
    Dock *dock,
    gint target_slot);

static void dock_update_drawer_reorder_indicator(
    DockDrawer *drawer);

static gboolean dock_drawer_reorder_indicator_draw(
    GtkWidget *widget,
    cairo_t *cr,
    gpointer user_data);

static void dock_update_reorder_indicator(
    Dock *dock);

static void dock_drawer_update_drop_states(
    Dock *dock,
    gint root_x,
    gint root_y);

static void dock_reset_drag_drawer_state(
    Dock *dock);

static void dock_item_free(gpointer data);

static void dock_save(Dock *dock);

static void dock_position_window(Dock *dock);
static void dock_monitor_cache_selected(
    Dock *dock,
    GdkMonitor *monitor);
static void dock_schedule_monitor_refresh(Dock *dock);
static gboolean dock_monitor_refresh_idle(gpointer user_data);
static void dock_display_monitor_changed(
    GdkDisplay *display,
    GdkMonitor *monitor,
    gpointer user_data);
static void dock_screen_monitors_changed(
    GdkScreen *screen,
    gpointer user_data);

static void dock_box_size_allocate(
    GtkWidget *widget,
    GtkAllocation *allocation,
    gpointer user_data);

static void dock_swap_side_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data);

static void dock_append_monitor_menu(
    GtkMenuShell *menu,
    Dock *dock);

static void dock_monitor_menu_item_toggled(
    GtkCheckMenuItem *menu_item,
    gpointer user_data);

static void dock_preferences_side_changed(
    GtkComboBox *combo,
    gpointer user_data);

static void dock_preferences_position_changed(
    GtkComboBox *combo,
    gpointer user_data);

static void dock_preferences_monitor_changed(
    GtkComboBox *combo,
    gpointer user_data);

static void dock_preferences_shortcut_only_toggled(
    GtkToggleButton *button,
    gpointer user_data);

static void dock_preferences_minimized_window_icons_toggled(
    GtkToggleButton *button,
    gpointer user_data);

static void dock_preferences_minimized_title_labels_toggled(
    GtkToggleButton *button,
    gpointer user_data);

static void dock_preferences_minimized_group_drawer_toggled(
    GtkToggleButton *button,
    gpointer user_data);

static void dock_preferences_minimized_all_workspaces_toggled(
    GtkToggleButton *button,
    gpointer user_data);

static void dock_preferences_indicator_toggled(
    GtkToggleButton *button,
    gpointer user_data);

static void dock_preferences_show_hide_handle_toggled(
    GtkToggleButton *button,
    gpointer user_data);

static void dock_preferences_icon_size_changed(
    GtkComboBox *combo,
    gpointer user_data);

static void dock_preferences_opacity_changed(
    GtkRange *range,
    gpointer user_data);

static void dock_preferences_bitmap_background_toggled(
    GtkToggleButton *button,
    gpointer user_data);

static void dock_preferences_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data);

static void dock_about_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data);

static void dock_quit_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data);

static void dock_edit_launcher_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data);

static void dock_edit_drawer_launcher_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data);

static void dock_edit_launcher(
    Dock *dock,
    DockItem *item,
    DockDrawerEntry *entry);

static gboolean dock_enter_notify(
    GtkWidget *widget,
    GdkEventCrossing *event,
    gpointer user_data);

static gboolean dock_leave_notify(
    GtkWidget *widget,
    GdkEventCrossing *event,
    gpointer user_data);

static gboolean dock_drawer_button_draw(
    GtkWidget *widget,
    cairo_t *cr,
    gpointer user_data);

static void dock_lower_window(
    Dock *dock);

static void dock_raise_window(
    Dock *dock);

static void dock_popup_menu_on_monitor(
    GtkMenu *menu,
    GtkWidget *widget,
    GdkEventButton *event,
    Dock *dock);

static void dock_drawer_open(DockDrawer *drawer);
static void dock_drawer_close(DockDrawer *drawer);

static gboolean dock_drawer_outside_poll(gpointer user_data);

static gboolean dock_drawer_hover_open(gpointer user_data);
static gboolean dock_drawer_hover_close(gpointer user_data);

static gboolean dock_drawer_enter_notify(
    GtkWidget *widget,
    GdkEventCrossing *event,
    gpointer user_data);

static gboolean dock_drawer_leave_notify(
    GtkWidget *widget,
    GdkEventCrossing *event,
    gpointer user_data);

static gboolean dock_drawer_animation_tick(
    GtkWidget *widget,
    GdkFrameClock *frame_clock,
    gpointer user_data);

static void dock_drawer_destroy_popup(
    DockDrawer *drawer);

static void dock_drawer_position_popup(
    DockDrawer *drawer,
    gdouble progress);

static gboolean dock_drawer_get_anchor(
    DockDrawer *drawer,
    gint *x,
    gint *y);

static gint dock_drawer_find_entry_index(
    DockDrawer *drawer,
    DockDrawerEntry *entry);

static gboolean dock_drawer_drag_tick(
    GtkWidget *widget,
    GdkFrameClock *frame_clock,
    gpointer user_data);

static void dock_drawer_begin_drag(
    DockDrawer *drawer,
    GtkWidget *button);

static void dock_drawer_update_drag(
    DockDrawer *drawer,
    gdouble root_x,
    gdouble root_y);

static void dock_drawer_end_drag(
    DockDrawer *drawer);

static gboolean dock_drawer_transfer_to_drawer(
    DockDrawer *source,
    DockDrawerEntry *entry,
    DockDrawer *target,
    gint target_index);

static void dock_update_drawer_transfer_indicator(
    DockDrawer *source,
    DockDrawer *target,
    gint target_slot);

static void dock_drawer_drag_proxy_destroy(
    DockDrawer *drawer);


static gboolean dock_drawer_point_inside_popup(
    DockDrawer *drawer,
    gint root_x,
    gint root_y);

static void dock_drawer_drag_proxy_update(
    DockDrawer *drawer,
    gboolean visible,
    gdouble root_x,
    gdouble root_y);

static guint dock_count_application_items(
    const Dock *dock);

static gboolean dock_button_press(
    GtkWidget *widget,
    GdkEventButton *event,
    gpointer user_data);

static gboolean dock_button_motion(
    GtkWidget *widget,
    GdkEventMotion *event,
    gpointer user_data);

static gboolean dock_button_release(
    GtkWidget *widget,
    GdkEventButton *event,
    gpointer user_data);

static void dock_enable_desktop_drop(
    GtkWidget *widget,
    Dock *dock);

static void dock_desktop_drag_data_received(
    GtkWidget *widget,
    GdkDragContext *context,
    gint x,
    gint y,
    GtkSelectionData *selection_data,
    guint info,
    guint time,
    gpointer user_data);

static void dock_drawer_desktop_drag_data_received(
    GtkWidget *widget,
    GdkDragContext *context,
    gint x,
    gint y,
    GtkSelectionData *selection_data,
    guint info,
    guint time,
    gpointer user_data);

static void dock_enable_desktop_drop_on_drawer(
    GtkWidget *widget,
    DockDrawer *drawer);

static gboolean dock_has_desktop_id(
    const Dock *dock,
    const gchar *desktop_id);

static void dock_move_to_dock_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data);

static gboolean dock_point_inside(
    Dock *dock,
    gint root_x,
    gint root_y,
    gint *slot_out);

static gboolean dock_drawer_transfer_to_dock(
    DockDrawer *drawer,
    DockDrawerEntry *entry,
    gint target_index);

static DockDrawer *dock_find_drawer_drop_target(
    Dock *dock,
    gint root_x,
    gint root_y,
    gint *target_index_out);

static DockDrawer *dock_find_drawer_tile_at_point(
    Dock *dock,
    gint root_x,
    gint root_y);

static void dock_update_drag_drawer_hover(
    Dock *dock,
    gint root_x,
    gint root_y);

static gboolean dock_transfer_to_drawer(
    Dock *dock,
    DockItem *item,
    DockDrawer *drawer,
    gint target_index);

static void
dock_drawer_entry_free(gpointer data)
{
    DockDrawerEntry *entry = data;

    if (!entry)
        return;

    g_free(entry->desktop_id);
    dock_launcher_free(entry->launcher);
    g_free(entry);
}

static gboolean
dock_drawer_has_desktop_id(
    DockDrawer *drawer,
    const gchar *desktop_id)
{
    for (guint i = 0; i < drawer->entries->len; i++) {
        DockDrawerEntry *entry =
            g_ptr_array_index(
                drawer->entries,
                i);

        if (g_strcmp0(
                entry->desktop_id,
                desktop_id) == 0)
            return TRUE;
    }

    return FALSE;
}

static void
dock_drawer_free(DockDrawer *drawer)
{
    if (!drawer)
        return;

    if (drawer->hover_open_id) {
        g_source_remove(drawer->hover_open_id);
        drawer->hover_open_id = 0;
    }

    if (drawer->hover_close_id) {
        g_source_remove(drawer->hover_close_id);
        drawer->hover_close_id = 0;
    }

    /*
     * The Drawer object is being destroyed here, so its popup must be
     * torn down synchronously rather than starting an animation whose
     * frame callback would retain a soon-to-be-freed DockDrawer.
     */
    dock_drawer_destroy_popup(drawer);

    g_clear_pointer(
        &drawer->entries,
        g_ptr_array_unref);

    g_free(drawer->name);
    g_free(drawer);
}

static void
dock_drawer_remove_entry(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    DockDrawer *drawer = user_data;
    DockDrawerEntry *entry =
        g_object_get_data(
            G_OBJECT(menu_item),
            "dock-drawer-entry");

    if (!entry)
        return;

    guint index = G_MAXUINT;

    for (guint i = 0;
         i < drawer->entries->len;
         i++) {
        if (g_ptr_array_index(
                drawer->entries,
                i) == entry) {
            index = i;
            break;
        }
    }

    if (index == G_MAXUINT)
        return;

    gboolean was_open =
        drawer->popup != NULL;

    g_ptr_array_remove_index(
        drawer->entries,
        index);

    if (was_open)
        dock_drawer_open(drawer);

    dock_save(drawer->dock);
}

static void
dock_drawer_launch_menu_item(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    DockDrawerEntry *entry = user_data;
    GtkWidget *button =
        g_object_get_data(
            G_OBJECT(menu_item),
            "dock-drawer-button");

    GError *error = NULL;

    if (!button ||
        !dock_launcher_launch(
            entry->launcher,
            button,
            gtk_get_current_event_time(),
            &error)) {
        g_warning(
            "Unable to launch Drawer application '%s': %s",
            dock_launcher_get_name(entry->launcher),
            error ? error->message : "unknown error");
        g_clear_error(&error);
    } else {
        dock_icon_button_set_launching(
            button,
            TRUE);
    }
}

static gboolean
dock_insertion_indicator_draw(
    GtkWidget *widget,
    cairo_t *cr,
    gpointer user_data)
{
    (void)user_data;

    gint edge =
        GPOINTER_TO_INT(
            g_object_get_data(
                G_OBJECT(widget),
                "dock-blue-insertion-edge"));

    if (edge == 0)
        return FALSE;

    GtkAllocation allocation;
    gtk_widget_get_allocation(
        widget,
        &allocation);

    if (allocation.width <= 2 ||
        allocation.height <= 2)
        return FALSE;

    gdouble y =
        edge == 2 ?
        allocation.height - 0.5 :
        0.5;

    cairo_save(cr);

    cairo_set_source_rgb(
        cr,
        0.0,
        0.0,
        1.0);

    cairo_set_line_width(
        cr,
        3.0);

    cairo_move_to(
        cr,
        1.0,
        y);

    cairo_line_to(
        cr,
        allocation.width - 1.0,
        y);

    cairo_stroke(cr);
    cairo_restore(cr);

    return FALSE;
}

static void
dock_update_drawer_drop_indicator(
    Dock *dock,
    gint target_slot)
{
    if (!dock ||
        !dock->items)
        return;

    for (guint i = 0;
         i < dock->items->len;
         i++) {
        DockItem *item =
            g_ptr_array_index(
                dock->items,
                i);

        if (!item ||
            !item->button)
            continue;

        g_object_set_data(
            G_OBJECT(item->button),
            "dock-blue-insertion-edge",
            GINT_TO_POINTER(0));
        gtk_widget_queue_draw(item->button);
    }

    if (target_slot < 0 ||
        dock->items->len == 0)
        return;

    DockItem *target = NULL;
    gint edge = 1;

    if (target_slot >= (gint)dock->items->len) {
        target =
            g_ptr_array_index(
                dock->items,
                dock->items->len - 1);
        edge = 2;
    } else {
        target =
            g_ptr_array_index(
                dock->items,
                (guint)target_slot);
    }

    if (!target ||
        !target->button)
        return;

    g_object_set_data(
        G_OBJECT(target->button),
        "dock-blue-insertion-edge",
        GINT_TO_POINTER(edge));

    gtk_widget_queue_draw(
        target->button);
}

static void
dock_update_reorder_indicator(
    Dock *dock)
{
    if (!dock ||
        !dock->items)
        return;

    for (guint i = 0;
         i < dock->items->len;
         i++) {
        DockItem *item =
            g_ptr_array_index(
                dock->items,
                i);

        if (!item ||
            !item->button)
            continue;

        g_object_set_data(
            G_OBJECT(item->button),
            "dock-blue-insertion-edge",
            GINT_TO_POINTER(0));
        gtk_widget_queue_draw(item->button);
    }

    if (!dock->dragging ||
        dock->source_index < 0 ||
        dock->target_index < 0 ||
        dock->source_index == dock->target_index ||
        dock->target_index >= (gint)dock->items->len)
        return;

    DockItem *target =
        g_ptr_array_index(
            dock->items,
            (guint)dock->target_index);

    if (!target ||
        !target->button)
        return;

    /*
     * target_index is the item's final slot after removing the source.
     * When moving downward, the dragged item ends up immediately after
     * the current target item; when moving upward, it ends up before it.
     */
    gint edge =
        dock->target_index > dock->source_index ?
        2 :
        1;

    g_object_set_data(
        G_OBJECT(target->button),
        "dock-blue-insertion-edge",
        GINT_TO_POINTER(edge));

    gtk_widget_queue_draw(
        target->button);
}

static gboolean
dock_drawer_reorder_indicator_draw(
    GtkWidget *widget,
    cairo_t *cr,
    gpointer user_data)
{
    (void)user_data;

    gint edge =
        GPOINTER_TO_INT(
            g_object_get_data(
                G_OBJECT(widget),
                "dock-drawer-blue-insertion-edge"));

    if (edge == 0)
        return FALSE;

    GtkAllocation allocation;
    gtk_widget_get_allocation(
        widget,
        &allocation);

    if (allocation.width <= 2 ||
        allocation.height <= 2)
        return FALSE;

    gdouble x =
        edge == 2 ?
        allocation.width - 0.5 :
        0.5;

    cairo_save(cr);

    cairo_set_source_rgb(
        cr,
        0.0,
        0.0,
        1.0);

    cairo_set_line_width(
        cr,
        3.0);

    cairo_move_to(
        cr,
        x,
        1.0);

    cairo_line_to(
        cr,
        x,
        allocation.height - 1.0);

    cairo_stroke(cr);
    cairo_restore(cr);

    return FALSE;
}

static void
dock_update_drawer_reorder_indicator(
    DockDrawer *drawer)
{
    if (!drawer ||
        !drawer->entries)
        return;

    for (guint i = 0;
         i < drawer->entries->len;
         i++) {
        DockDrawerEntry *entry =
            g_ptr_array_index(
                drawer->entries,
                i);

        if (!entry ||
            !entry->button)
            continue;

        g_object_set_data(
            G_OBJECT(entry->button),
            "dock-drawer-blue-insertion-edge",
            GINT_TO_POINTER(0));

        gtk_widget_queue_draw(
            entry->button);
    }

    if (!drawer->dragging ||
        drawer->source_index < 0 ||
        drawer->target_index < 0 ||
        drawer->source_index == drawer->target_index ||
        drawer->target_index >= (gint)drawer->entries->len)
        return;

    DockDrawerEntry *target =
        g_ptr_array_index(
            drawer->entries,
            (guint)drawer->target_index);

    if (!target ||
        !target->button)
        return;

    gint edge =
        drawer->target_index > drawer->source_index ?
        2 :
        1;

    g_object_set_data(
        G_OBJECT(target->button),
        "dock-drawer-blue-insertion-edge",
        GINT_TO_POINTER(edge));

    gtk_widget_queue_draw(
        target->button);
}

static gboolean
dock_drawer_button_draw(
    GtkWidget *widget,
    cairo_t *cr,
    gpointer user_data)
{
    (void)user_data;

    if (!(gtk_widget_get_state_flags(widget) &
          GTK_STATE_FLAG_DROP_ACTIVE))
        return FALSE;

    GtkAllocation allocation;
    gtk_widget_get_allocation(
        widget,
        &allocation);

    if (allocation.width <= 2 ||
        allocation.height <= 2)
        return FALSE;

    cairo_save(cr);

    cairo_set_source_rgb(
        cr,
        1.0,
        0.0,
        0.0);

    cairo_set_line_width(
        cr,
        1.0);

    cairo_rectangle(
        cr,
        1.0,
        1.0,
        allocation.width - 2.0,
        allocation.height - 2.0);

    cairo_stroke(cr);

    cairo_restore(cr);

    return FALSE;
}

static gboolean
dock_drawer_entry_button_press(
    GtkWidget *widget,
    GdkEventButton *event,
    gpointer user_data)
{
    DockDrawer *drawer = user_data;
    DockDrawerEntry *entry =
        g_object_get_data(
            G_OBJECT(widget),
            "dock-drawer-entry");

    if (!entry)
        return FALSE;

    if (event->button == GDK_BUTTON_SECONDARY) {
        GtkWidget *menu =
            gtk_menu_new();

    gtk_menu_set_reserve_toggle_size(
        GTK_MENU(menu),
        FALSE);

        GtkWidget *open =
            dock_menu_item_new(
            "Launch New Instance",
            "system-run");

        g_object_set_data(
            G_OBJECT(open),
            "dock-drawer-button",
            widget);

        g_signal_connect(
            open,
            "activate",
            G_CALLBACK(
                dock_drawer_launch_menu_item),
            entry);

        gtk_menu_shell_append(
            GTK_MENU_SHELL(menu),
            open);

        GtkWidget *edit =
            dock_menu_item_new(
                "Edit Launcher",
                "document-edit");

        g_object_set_data(
            G_OBJECT(edit),
            "dock-drawer-entry",
            entry);

        g_signal_connect(
            edit,
            "activate",
            G_CALLBACK(
                dock_edit_drawer_launcher_menu_item_activated),
            drawer->dock);

        gtk_menu_shell_append(
            GTK_MENU_SHELL(menu),
            edit);

        if (drawer->dock->x11) {
            dock_x11_append_window_menu(
                drawer->dock->x11,
                widget,
                GTK_MENU_SHELL(menu));
        }

        if (!dock_has_desktop_id(
                drawer->dock,
                entry->desktop_id)) {
            GtkWidget *move =
                dock_menu_item_new(
            "Move to Dock",
            "go-home");

            g_object_set_data(
                G_OBJECT(move),
                "dock-move-drawer",
                drawer);

            g_object_set_data(
                G_OBJECT(move),
                "dock-move-entry",
                entry);

            g_signal_connect(
                move,
                "activate",
                G_CALLBACK(
                    dock_move_to_dock_menu_item_activated),
                drawer->dock);

            gtk_menu_shell_append(
                GTK_MENU_SHELL(menu),
                move);
        }

        gtk_menu_shell_append(
            GTK_MENU_SHELL(menu),
            gtk_separator_menu_item_new());

        GtkWidget *remove =
            dock_menu_item_new(
            "Remove from Drawer",
            "list-remove");

        g_object_set_data(
            G_OBJECT(remove),
            "dock-drawer-entry",
            entry);

        g_signal_connect(
            remove,
            "activate",
            G_CALLBACK(dock_drawer_remove_entry),
            drawer);

        gtk_menu_shell_append(
            GTK_MENU_SHELL(menu),
            remove);

        gtk_widget_show_all(menu);

        dock_popup_menu_on_monitor(
            GTK_MENU(menu),
            widget,
            event,
            drawer->dock);

        return TRUE;
    }

    if (event->button == GDK_BUTTON_MIDDLE) {
        GError *error = NULL;

        if (!dock_launcher_launch(
                entry->launcher,
                widget,
                event->time,
                &error)) {
            g_warning(
                "Unable to launch new Drawer application instance '%s': %s",
                dock_launcher_get_name(entry->launcher),
                error ? error->message : "unknown error");
            g_clear_error(&error);
        }

        return TRUE;
    }

    if (event->button == GDK_BUTTON_PRIMARY) {
        if (drawer->animation_closing)
            return TRUE;

        /*
         * Allow a primary press during the last part of the opening
         * animation. The popup is already present and the normal release
         * path will activate the application once the press is complete.
         */
        if (drawer->animation_progress < 0.25)
            return TRUE;

        drawer->drag_press_active = TRUE;
        drawer->dragging = FALSE;
        drawer->dropping = FALSE;
        drawer->drag_button = widget;
        drawer->drag_entry = entry;
        drawer->press_root_x = event->x_root;
        drawer->press_root_y = event->y_root;

        return TRUE;
    }

    return FALSE;
}

static gboolean
dock_drawer_entry_motion(
    GtkWidget *widget,
    GdkEventMotion *event,
    gpointer user_data)
{
    DockDrawer *drawer = user_data;

    if (!drawer->drag_press_active ||
        drawer->drag_button != widget ||
        drawer->animation_closing)
        return FALSE;

    if (!drawer->dragging) {
        gdouble distance =
            hypot(
                event->x_root - drawer->press_root_x,
                event->y_root - drawer->press_root_y);

        if (distance >= 5.0) {
            dock_drawer_begin_drag(
                drawer,
                widget);
        }
    }

    if (drawer->dragging)
        dock_drawer_update_drag(
            drawer,
            event->x_root,
            event->y_root);

    return TRUE;
}

static gboolean
dock_drawer_entry_release(
    GtkWidget *widget,
    GdkEventButton *event,
    gpointer user_data)
{
    DockDrawer *drawer = user_data;

    if (event->button != GDK_BUTTON_PRIMARY ||
        !drawer->drag_press_active ||
        drawer->drag_button != widget)
        return FALSE;

    drawer->drag_press_active = FALSE;
    drawer->drag_button = NULL;

    DockDrawerEntry *entry =
        drawer->drag_entry;

    if (drawer->dragging) {
        gint target_index = -1;
        gboolean over_dock =
            dock_point_inside(
                drawer->dock,
                event->x_root,
                event->y_root,
                &target_index);

        /*
         * Use the exact Dock insertion slot that drove the blue marker.
         * Recomputing it from the release event can disagree by a few
         * pixels after crossing between X11 windows.
         */
        if (drawer->dock->drawer_drop_on_dock)
            target_index =
                drawer->dock->drawer_drop_dock_slot;

        gint target_drawer_slot = -1;
        DockDrawer *target_drawer =
            dock_find_drawer_drop_target(
                drawer->dock,
                (gint)event->x_root,
                (gint)event->y_root,
                &target_drawer_slot);

        /*
         * A drop on another Drawer tile takes precedence over the Dock
         * beneath it. Dropping back onto the source Drawer is not a Dock
         * transfer, either.
         */
        if (target_drawer == drawer) {
            target_drawer = NULL;
            target_drawer_slot = -1;
            over_dock = FALSE;
        }

        DockDndDropTarget drop_target =
            dock_dnd_resolve_drop_target(
                over_dock,
                target_drawer != NULL,
                target_drawer_slot,
                target_index);

        gtk_grab_remove(widget);

        if (drop_target.kind == DOCK_DND_DROP_DRAWER &&
            target_drawer &&
            entry &&
            dock_drawer_transfer_to_drawer(
                drawer,
                entry,
                target_drawer,
                drop_target.slot)) {
            dock_update_drawer_transfer_indicator(
                drawer,
                NULL,
                -1);

            drawer->drag_entry = NULL;
            drawer->drag_button = NULL;
            drawer->dragging = FALSE;
            drawer->dropping = FALSE;
            drawer->source_index = -1;
            drawer->target_index = -1;
            drawer->drag_target_drawer = NULL;
            drawer->drag_target_slot = -1;
            drawer->drag_target_x = 0.0;
            drawer->drag_last_frame_us = 0;

            if (drawer->drag_tick_id &&
                drawer->dock &&
                drawer->dock->box) {
                gtk_widget_remove_tick_callback(
                    drawer->dock->box,
                    drawer->drag_tick_id);
                drawer->drag_tick_id = 0;
            }

            dock_drawer_drag_proxy_destroy(drawer);

            return TRUE;
        }

        if (drop_target.kind == DOCK_DND_DROP_DOCK &&
            entry &&
            dock_drawer_transfer_to_dock(
                drawer,
                entry,
                drop_target.slot)) {
            drawer->dock->drawer_drop_on_dock = FALSE;
            drawer->dock->drawer_drop_dock_slot = -1;
            dock_update_drawer_drop_indicator(
                drawer->dock,
                -1);

            drawer->drag_entry = NULL;
            drawer->drag_button = NULL;
            drawer->dragging = FALSE;
            drawer->dropping = FALSE;
            drawer->drag_target_drawer = NULL;
            drawer->drag_target_slot = -1;

            if (drawer->drag_tick_id &&
                drawer->dock &&
                drawer->dock->box) {
                gtk_widget_remove_tick_callback(
                    drawer->dock->box,
                    drawer->drag_tick_id);
                drawer->drag_tick_id = 0;
            }

            dock_drawer_drag_proxy_destroy(drawer);

            return TRUE;
        }

        dock_drawer_end_drag(drawer);
        return TRUE;
    }

    drawer->drag_entry = NULL;

    gboolean activated = FALSE;

    if (drawer->dock->x11) {
        activated =
            dock_x11_activate_button(
                drawer->dock->x11,
                widget,
                event->time,
                dock_x11_get_active_window(
                    drawer->dock->x11));
    }

    if (!activated) {
        GError *error = NULL;

        if (!dock_launcher_launch(
                entry->launcher,
                widget,
                event->time,
                &error)) {
            g_warning(
                "Unable to launch Drawer application '%s': %s",
                dock_launcher_get_name(entry->launcher),
                error ? error->message : "unknown error");
            g_clear_error(&error);
        } else {
            dock_icon_button_set_launching(
                widget,
                TRUE);
        }
    }

    return TRUE;
}



static gint
dock_drawer_find_entry_index(
    DockDrawer *drawer,
    DockDrawerEntry *entry)
{
    for (guint i = 0;
         i < drawer->entries->len;
         i++) {
        if (g_ptr_array_index(
                drawer->entries,
                i) == entry)
            return (gint)i;
    }

    return -1;
}

static void
dock_drawer_apply_entry_position(
    GtkFixed *fixed,
    DockDrawerEntry *entry)
{
    gint x =
        (gint)round(entry->visual_x);

    if (x == entry->applied_x)
        return;

    gtk_fixed_move(
        fixed,
        entry->button,
        x,
        0);

    entry->applied_x = x;
}

static void
dock_drawer_set_neighbor_targets(
    DockDrawer *drawer,
    gint source_index,
    gint target_index)
{
    for (guint i = 0;
         i < drawer->entries->len;
         i++) {
        DockDrawerEntry *entry =
            g_ptr_array_index(
                drawer->entries,
                i);

        if ((gint)i == source_index)
            continue;

        gint slot = (gint)i;

        if (target_index > source_index &&
            slot > source_index &&
            slot <= target_index) {
            slot--;
        } else if (
            target_index < source_index &&
            slot >= target_index &&
            slot < source_index) {
            slot++;
        }

        entry->target_x =
            slot * DOCK_TILE_SIZE;
    }
}

static gboolean
dock_drawer_drag_tick(
    GtkWidget *widget,
    GdkFrameClock *frame_clock,
    gpointer user_data)
{
    (void)widget;

    DockDrawer *drawer = user_data;

    if (!drawer->drag_entry ||
        !drawer->popup)
        return G_SOURCE_REMOVE;

    gint64 now_us =
        gdk_frame_clock_get_frame_time(
            frame_clock);

    if (drawer->drag_last_frame_us == 0)
        drawer->drag_last_frame_us = now_us;

    gdouble dt =
        (gdouble)(
            now_us -
            drawer->drag_last_frame_us) /
        1000000.0;

    drawer->drag_last_frame_us = now_us;
    dt = CLAMP(dt, 0.0, 0.05);

    gdouble neighbor_blend =
        1.0 -
        exp(-DRAG_NEIGHBOR_RATE * dt);

    gdouble drag_blend =
        1.0 -
        exp(-DRAG_FOLLOW_RATE * dt);

    if (drawer->drag_proxy &&
        drawer->drag_proxy_visible) {
        gdouble proxy_blend =
            1.0 -
            exp(-DRAG_FOLLOW_RATE * dt);

        drawer->drag_proxy_x +=
            (drawer->drag_proxy_target_x -
             drawer->drag_proxy_x) *
            proxy_blend;

        drawer->drag_proxy_y +=
            (drawer->drag_proxy_target_y -
             drawer->drag_proxy_y) *
            proxy_blend;

        gtk_window_move(
            GTK_WINDOW(drawer->drag_proxy),
            (gint)round(drawer->drag_proxy_x),
            (gint)round(drawer->drag_proxy_y));
    }

    GtkWidget *fixed_widget =
        gtk_bin_get_child(
            GTK_BIN(drawer->popup));

    if (!fixed_widget ||
        !GTK_IS_FIXED(fixed_widget))
        return G_SOURCE_REMOVE;

    GtkFixed *fixed =
        GTK_FIXED(fixed_widget);

    DockDrawerEntry *drag_entry =
        drawer->drag_entry;

    if (drawer->dragging &&
        !drawer->dropping) {
        gdouble delta =
            drawer->drag_target_x -
            drag_entry->visual_x;

        if (fabs(delta) <= 0.1)
            drag_entry->visual_x =
                drawer->drag_target_x;
        else
            drag_entry->visual_x +=
                delta * drag_blend;

        dock_drawer_apply_entry_position(
            fixed,
            drag_entry);
    }

    for (guint i = 0;
         i < drawer->entries->len;
         i++) {
        DockDrawerEntry *entry =
            g_ptr_array_index(
                drawer->entries,
                i);

        if (entry == drag_entry)
            continue;

        gdouble delta =
            entry->target_x -
            entry->visual_x;

        if (fabs(delta) <= 0.1)
            entry->visual_x =
                entry->target_x;
        else
            entry->visual_x +=
                delta * neighbor_blend;

        dock_drawer_apply_entry_position(
            fixed,
            entry);
    }

    if (drawer->dropping) {
        gint target_index =
            CLAMP(
                drawer->target_index,
                0,
                MAX(
                    0,
                    (gint)drawer->entries->len - 1));

        gdouble target_x =
            target_index * DOCK_TILE_SIZE;

        gdouble delta =
            target_x -
            drag_entry->visual_x;

        if (fabs(delta) <= 0.1)
            drag_entry->visual_x =
                target_x;
        else
            drag_entry->visual_x +=
                delta * drag_blend;

        dock_drawer_apply_entry_position(
            fixed,
            drag_entry);

        gboolean settled =
            TRUE;

        for (guint i = 0;
             i < drawer->entries->len;
             i++) {
            DockDrawerEntry *entry =
                g_ptr_array_index(
                    drawer->entries,
                    i);

            if (fabs(
                    entry->visual_x -
                    entry->target_x) > 0.25) {
                settled = FALSE;
                break;
            }
        }

        if (settled) {
            drag_entry->visual_x = target_x;
            dock_drawer_apply_entry_position(
                fixed,
                drag_entry);

            drawer->drag_entry = NULL;
            drawer->drag_button = NULL;
            drawer->dragging = FALSE;
            drawer->dropping = FALSE;
            drawer->source_index = -1;
            drawer->target_index = -1;
            drawer->drag_target_x = 0.0;
            drawer->drag_last_frame_us = 0;

            dock_drawer_drag_proxy_destroy(drawer);

            drawer->drag_tick_id = 0;
            return G_SOURCE_REMOVE;
        }
    }

    return G_SOURCE_CONTINUE;
}

static gboolean
dock_drawer_point_inside_popup(
    DockDrawer *drawer,
    gint root_x,
    gint root_y)
{
    if (!drawer ||
        !drawer->popup)
        return FALSE;

    gint window_x = 0;
    gint window_y = 0;

    gtk_window_get_position(
        GTK_WINDOW(drawer->popup),
        &window_x,
        &window_y);

    GtkAllocation allocation;
    gtk_widget_get_allocation(
        drawer->popup,
        &allocation);

    return
        root_x >= window_x &&
        root_y >= window_y &&
        root_x < window_x + allocation.width &&
        root_y < window_y + allocation.height;
}

static void
dock_drawer_drag_proxy_destroy(
    DockDrawer *drawer)
{
    if (!drawer)
        return;

    if (drawer->drag_proxy) {
        gtk_widget_destroy(
            drawer->drag_proxy);
        drawer->drag_proxy = NULL;
    }

    drawer->drag_proxy_visible = FALSE;
    drawer->drag_proxy_x = 0.0;
    drawer->drag_proxy_y = 0.0;
    drawer->drag_proxy_target_x = 0.0;
    drawer->drag_proxy_target_y = 0.0;
}

static void
dock_drawer_drag_proxy_update(
    DockDrawer *drawer,
    gboolean visible,
    gdouble root_x,
    gdouble root_y)
{
    if (!drawer ||
        !drawer->drag_entry ||
        !drawer->drag_entry->launcher)
        return;

    if (!drawer->drag_proxy) {
        GtkWidget *popup =
            gtk_window_new(
                GTK_WINDOW_POPUP);

        gtk_window_set_decorated(
            GTK_WINDOW(popup),
            FALSE);
        gtk_window_set_resizable(
            GTK_WINDOW(popup),
            FALSE);
        gtk_window_set_skip_taskbar_hint(
            GTK_WINDOW(popup),
            TRUE);
        gtk_window_set_skip_pager_hint(
            GTK_WINDOW(popup),
            TRUE);
        gtk_window_set_type_hint(
            GTK_WINDOW(popup),
            GDK_WINDOW_TYPE_HINT_DOCK);
        gtk_window_set_accept_focus(
            GTK_WINDOW(popup),
            FALSE);
        gtk_window_set_focus_on_map(
            GTK_WINDOW(popup),
            FALSE);
        gtk_widget_set_opacity(
            popup,
            drawer->dock->opacity_percent / 100.0);

        GtkWidget *button =
            dock_icon_button_new(
                drawer->drag_entry->launcher);

        if (!button) {
            gtk_widget_destroy(popup);
            return;
        }

        dock_icon_button_set_icon_size(
            button,
            drawer->dock->icon_size);

        gtk_widget_set_sensitive(
            button,
            FALSE);
        gtk_widget_set_focus_on_click(
            button,
            FALSE);
        gtk_widget_set_size_request(
            button,
            DOCK_TILE_SIZE,
            DOCK_TILE_SIZE);

        gtk_container_add(
            GTK_CONTAINER(popup),
            button);

        drawer->drag_proxy = popup;
        drawer->drag_proxy_x =
            root_x - DOCK_TILE_SIZE / 2.0;
        drawer->drag_proxy_y =
            root_y - DOCK_TILE_SIZE / 2.0;
        drawer->drag_proxy_target_x =
            drawer->drag_proxy_x;
        drawer->drag_proxy_target_y =
            drawer->drag_proxy_y;
    }

    drawer->drag_proxy_target_x =
        root_x - DOCK_TILE_SIZE / 2.0;
    drawer->drag_proxy_target_y =
        root_y - DOCK_TILE_SIZE / 2.0;

    if (visible) {
        if (!drawer->drag_proxy_visible) {
            drawer->drag_proxy_visible = TRUE;
            gtk_widget_show_all(
                drawer->drag_proxy);
        }
    } else if (drawer->drag_proxy_visible) {
        drawer->drag_proxy_visible = FALSE;
        gtk_widget_hide(
            drawer->drag_proxy);
    }
}

static void
dock_drawer_ensure_drag_tick(
    DockDrawer *drawer)
{
    if (!drawer ||
        !drawer->dock ||
        !drawer->dock->box ||
        drawer->drag_tick_id)
        return;

    drawer->drag_last_frame_us = 0;

    drawer->drag_tick_id =
        gtk_widget_add_tick_callback(
            drawer->dock->box,
            dock_drawer_drag_tick,
            drawer,
            NULL);
}

static void
dock_drawer_begin_drag(
    DockDrawer *drawer,
    GtkWidget *button)
{
    DockDrawerEntry *entry =
        g_object_get_data(
            G_OBJECT(button),
            "dock-drawer-entry");

    if (!entry ||
        drawer->dragging ||
        drawer->dropping)
        return;

    gint source_index =
        dock_drawer_find_entry_index(
            drawer,
            entry);

    if (source_index < 0)
        return;

    drawer->dragging = TRUE;
    drawer->dropping = FALSE;
    drawer->drag_button = button;
    drawer->drag_entry = entry;
    drawer->source_index = source_index;
    drawer->target_index = source_index;
    drawer->drag_target_drawer = NULL;
    drawer->drag_target_slot = -1;
    drawer->drag_target_x =
        source_index * DOCK_TILE_SIZE;

    g_object_set_data(
        G_OBJECT(button),
        "dock-drawer-suppress-click",
        GINT_TO_POINTER(TRUE));

    if (drawer->hover_open_id) {
        g_source_remove(
            drawer->hover_open_id);
        drawer->hover_open_id = 0;
    }

    if (drawer->hover_close_id) {
        g_source_remove(
            drawer->hover_close_id);
        drawer->hover_close_id = 0;
    }

    /*
     * Keep receiving motion/release events after the pointer crosses out
     * of the Drawer popup. This is what makes a real cross-window drag
     * possible on X11.
     */
    gtk_grab_add(button);

    drawer->drag_last_frame_us = 0;

    dock_drawer_ensure_drag_tick(
        drawer);
}

static void
dock_drawer_update_drag(
    DockDrawer *drawer,
    gdouble root_x,
    gdouble root_y)
{
    if (!drawer->dragging ||
        !drawer->drag_entry)
        return;

    Dock *dock = drawer->dock;
    gint dock_slot = -1;
    gboolean pointer_over_dock =
        dock_point_inside(
            dock,
            (gint)root_x,
            (gint)root_y,
            &dock_slot);
    gboolean over_dock = pointer_over_dock;

    gint target_slot = -1;
    DockDrawer *target_drawer =
        dock_find_drawer_drop_target(
            dock,
            (gint)root_x,
            (gint)root_y,
            &target_slot);

    /*
     * Drawer tiles live inside the Dock window, so resolve the Drawer hit
     * before the Dock hit. Otherwise a cross-Drawer drop onto a closed
     * Drawer tile would incorrectly move the launcher onto the Dock.
     */
    if (target_drawer == drawer) {
        target_drawer = NULL;
        target_slot = -1;
        over_dock = FALSE;
    }

    DockDndDropTarget drop_target =
        dock_dnd_resolve_drop_target(
            over_dock,
            target_drawer != NULL,
            target_slot,
            dock_slot);

    over_dock =
        drop_target.kind == DOCK_DND_DROP_DOCK;

    if (drop_target.kind != DOCK_DND_DROP_DRAWER) {
        target_drawer = NULL;
        target_slot = -1;
    } else {
        target_slot = drop_target.slot;
    }

    gboolean inside_popup =
        dock_drawer_point_inside_popup(
            drawer,
            (gint)root_x,
            (gint)root_y);

    dock_drawer_drag_proxy_update(
        drawer,
        !inside_popup && !pointer_over_dock,
        root_x,
        root_y);

    if (over_dock) {
        if (!dock->drawer_drop_on_dock ||
            dock->drawer_drop_dock_slot != dock_slot) {
            dock->drawer_drop_on_dock = TRUE;
            dock->drawer_drop_dock_slot = dock_slot;
            dock_update_drawer_drop_indicator(
                dock,
                dock_slot);
        }
    } else if (dock->drawer_drop_on_dock) {
        dock->drawer_drop_on_dock = FALSE;
        dock->drawer_drop_dock_slot = -1;
        dock_update_drawer_drop_indicator(
            dock,
            -1);
    }

    /*
     * A hovered Drawer opens after the same short delay used by Dock
     * application drags. This makes a closed destination Drawer usable
     * without requiring the drag to pass through its popup.
     */
    DockDrawer *hover_target =
        dock_find_drawer_tile_at_point(
            dock,
            (gint)root_x,
            (gint)root_y);

    if (!hover_target) {
        hover_target =
            dock_find_drawer_drop_target(
                dock,
                (gint)root_x,
                (gint)root_y,
                NULL);
    }

    if (hover_target != dock->drag_hover_drawer) {
        if (dock->drag_hover_drawer &&
            dock->drag_hover_drawer->hover_open_id) {
            g_source_remove(
                dock->drag_hover_drawer->hover_open_id);
            dock->drag_hover_drawer->hover_open_id = 0;
        }

        dock->drag_hover_drawer =
            hover_target;

        if (hover_target &&
            hover_target != drawer &&
            !hover_target->popup &&
            !hover_target->animation_closing) {
            hover_target->hover_open_id =
                g_timeout_add(
                    DRAWER_DRAG_HOVER_OPEN_DELAY_MS,
                    dock_drawer_hover_open,
                    hover_target);
        }
    }

    drawer->drag_target_drawer =
        target_drawer;
    drawer->drag_target_slot =
        target_slot;

    if (target_drawer) {
        drawer->target_index =
            drawer->source_index;
        drawer->drag_target_x =
            drawer->source_index *
            DOCK_TILE_SIZE;

        dock_drawer_set_neighbor_targets(
            drawer,
            drawer->source_index,
            drawer->source_index);

        dock_update_drawer_reorder_indicator(
            drawer);

        dock_update_drawer_transfer_indicator(
            drawer,
            target_drawer,
            target_slot);

        dock_drawer_ensure_drag_tick(
            drawer);
        return;
    }

    dock_update_drawer_transfer_indicator(
        drawer,
        NULL,
        -1);

    gint count =
        (gint)drawer->entries->len;

    if (count <= 0)
        return;

    gdouble drag_x =
        drawer->source_index *
        DOCK_TILE_SIZE +
        (root_x -
         drawer->press_root_x);

    drawer->drag_target_x =
        CLAMP(
            drag_x,
            0.0,
            (gdouble)(
                (count - 1) *
                DOCK_TILE_SIZE));

    gint target_index =
        CLAMP(
            (gint)floor(
                (drawer->drag_target_x +
                 DOCK_TILE_SIZE / 2.0) /
                DOCK_TILE_SIZE),
            0,
            count - 1);

    if (target_index != drawer->target_index) {
        drawer->target_index =
            target_index;

        dock_drawer_set_neighbor_targets(
            drawer,
            drawer->source_index,
            target_index);
    }

    dock_update_drawer_reorder_indicator(
        drawer);

    dock_drawer_ensure_drag_tick(
        drawer);
}

static void
dock_update_drawer_transfer_indicator(
    DockDrawer *source,
    DockDrawer *target,
    gint target_slot)
{
    if (!source ||
        !source->dock ||
        !source->dock->items)
        return;

    for (guint i = 0;
         i < source->dock->items->len;
         i++) {
        DockItem *item =
            g_ptr_array_index(
                source->dock->items,
                i);

        if (!item ||
            !item->is_drawer ||
            !item->drawer ||
            !item->drawer->entries)
            continue;

        gtk_widget_unset_state_flags(
            item->drawer->button,
            GTK_STATE_FLAG_DROP_ACTIVE);

        for (guint j = 0;
             j < item->drawer->entries->len;
             j++) {
            DockDrawerEntry *entry =
                g_ptr_array_index(
                    item->drawer->entries,
                    j);

            if (!entry || !entry->button)
                continue;

            g_object_set_data(
                G_OBJECT(entry->button),
                "dock-drawer-blue-insertion-edge",
                GINT_TO_POINTER(0));
            gtk_widget_queue_draw(
                entry->button);
        }
    }

    if (!target ||
        target == source ||
        target_slot < 0)
        return;

    if (target->entries->len == 0) {
        gtk_widget_set_state_flags(
            target->button,
            GTK_STATE_FLAG_DROP_ACTIVE,
            TRUE);
        return;
    }

    DockDrawerEntry *highlight = NULL;
    gint edge = 1;

    if (target_slot >= (gint)target->entries->len) {
        highlight =
            g_ptr_array_index(
                target->entries,
                target->entries->len - 1);
        edge = 2;
    } else {
        highlight =
            g_ptr_array_index(
                target->entries,
                (guint)target_slot);
    }

    if (!highlight ||
        !highlight->button)
        return;

    g_object_set_data(
        G_OBJECT(highlight->button),
        "dock-drawer-blue-insertion-edge",
        GINT_TO_POINTER(edge));
    gtk_widget_queue_draw(
        highlight->button);
}

static gboolean
dock_drawer_transfer_to_drawer(
    DockDrawer *source,
    DockDrawerEntry *entry,
    DockDrawer *target,
    gint target_index)
{
    if (!source ||
        !target ||
        source == target ||
        !entry ||
        !source->entries ||
        !target->entries ||
        !entry->desktop_id)
        return FALSE;

    if (dock_drawer_has_desktop_id(
            target,
            entry->desktop_id))
        return FALSE;

    gint source_index =
        dock_drawer_find_entry_index(
            source,
            entry);

    if (source_index < 0)
        return FALSE;

    gboolean source_was_open =
        source->popup != NULL;
    gboolean target_was_open =
        target->popup != NULL;

    DockDrawerEntry *moved =
        g_ptr_array_steal_index(
            source->entries,
            (guint)source_index);

    gint count =
        (gint)target->entries->len;

    gint insert_index =
        CLAMP(
            target_index,
            0,
            count);

    g_ptr_array_insert(
        target->entries,
        (guint)insert_index,
        moved);

    moved->button = NULL;
    moved->visual_x = 0.0;
    moved->target_x = 0.0;
    moved->applied_x = -1;

    if (source_was_open)
        dock_drawer_open(source);

    if (target_was_open)
        dock_drawer_open(target);

    dock_save(source->dock);

    return TRUE;
}

static void
dock_drawer_end_drag(
    DockDrawer *drawer)
{
    if (!drawer->dragging ||
        !drawer->drag_entry)
        return;

    if (drawer->drag_button)
        gtk_grab_remove(
            drawer->drag_button);

    dock_drawer_drag_proxy_destroy(drawer);
    dock_update_drawer_reorder_indicator(drawer);
    dock_update_drawer_transfer_indicator(
        drawer,
        NULL,
        -1);

    drawer->dock->drawer_drop_on_dock = FALSE;
    drawer->dock->drawer_drop_dock_slot = -1;
    dock_update_drawer_drop_indicator(
        drawer->dock,
        -1);

    drawer->dock->drag_hover_drawer = NULL;

    DockDrawer *target_drawer =
        drawer->drag_target_drawer;
    gint target_slot =
        drawer->drag_target_slot;

    if (target_drawer &&
        target_drawer != drawer) {
        DockDrawerEntry *entry =
            drawer->drag_entry;

        if (dock_drawer_transfer_to_drawer(
                drawer,
                entry,
                target_drawer,
                target_slot)) {
            drawer->drag_entry = NULL;
            drawer->drag_button = NULL;
            drawer->dragging = FALSE;
            drawer->dropping = FALSE;
            drawer->source_index = -1;
            drawer->target_index = -1;
            drawer->drag_target_drawer = NULL;
            drawer->drag_target_slot = -1;
            drawer->drag_target_x = 0.0;
            drawer->drag_last_frame_us = 0;

            if (drawer->drag_tick_id &&
                drawer->dock &&
                drawer->dock->box) {
                gtk_widget_remove_tick_callback(
                    drawer->dock->box,
                    drawer->drag_tick_id);
                drawer->drag_tick_id = 0;
            }

            return;
        }
    }

    gint source_index =
        drawer->source_index;

    gint target_index =
        drawer->target_index;

    if (source_index >= 0 &&
        target_index >= 0 &&
        source_index != target_index) {
        DockDrawerEntry *entry =
            g_ptr_array_steal_index(
                drawer->entries,
                (guint)source_index);

        g_ptr_array_insert(
            drawer->entries,
            (guint)target_index,
            entry);

        dock_save(
            drawer->dock);
    }

    for (guint i = 0;
         i < drawer->entries->len;
         i++) {
        DockDrawerEntry *entry =
            g_ptr_array_index(
                drawer->entries,
                i);

        entry->target_x =
            i * DOCK_TILE_SIZE;
    }

    drawer->drag_target_drawer = NULL;
    drawer->drag_target_slot = -1;
    drawer->dropping = TRUE;
    dock_drawer_ensure_drag_tick(
        drawer);
}

static void
dock_drawer_popup_add_entry(
    DockDrawer *drawer,
    GtkFixed *fixed,
    DockDrawerEntry *entry,
    guint index)
{
    GtkWidget *button =
        dock_icon_button_new(
            entry->launcher);

    if (!button)
        return;

    dock_icon_button_set_icon_size(
        button,
        drawer->dock->icon_size);

    gtk_widget_set_size_request(
        button,
        DOCK_TILE_SIZE,
        DOCK_TILE_SIZE);

    entry->button = button;
    entry->visual_x =
        index * DOCK_TILE_SIZE;
    entry->target_x =
        entry->visual_x;
    entry->applied_x = -1;

    gtk_fixed_put(
        fixed,
        button,
        (gint)entry->visual_x,
        0);

    dock_enable_desktop_drop_on_drawer(
        button,
        drawer);

    g_object_set_data(
        G_OBJECT(button),
        "dock-drawer-entry",
        entry);

    g_signal_connect_after(
        button,
        "draw",
        G_CALLBACK(dock_drawer_reorder_indicator_draw),
        NULL);

    gtk_widget_set_tooltip_text(
        button,
        dock_launcher_get_name(
            entry->launcher));

    gtk_widget_add_events(
        button,
        GDK_BUTTON_PRESS_MASK |
        GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK);

    g_signal_connect(
        button,
        "button-press-event",
        G_CALLBACK(dock_drawer_entry_button_press),
        drawer);

    g_signal_connect(
        button,
        "motion-notify-event",
        G_CALLBACK(dock_drawer_entry_motion),
        drawer);

    g_signal_connect_after(
        button,
        "button-release-event",
        G_CALLBACK(dock_drawer_entry_release),
        drawer);

    gtk_widget_show_all(button);
}

static void
dock_drawer_rebuild_popup(
    DockDrawer *drawer)
{
    dock_drawer_destroy_popup(drawer);

    GtkWidget *popup =
        gtk_window_new(
            GTK_WINDOW_POPUP);

    gtk_window_set_decorated(
        GTK_WINDOW(popup),
        FALSE);
    gtk_window_set_resizable(
        GTK_WINDOW(popup),
        FALSE);
    gtk_window_set_skip_taskbar_hint(
        GTK_WINDOW(popup),
        TRUE);
    gtk_window_set_skip_pager_hint(
        GTK_WINDOW(popup),
        TRUE);
    gtk_window_set_type_hint(
        GTK_WINDOW(popup),
        GDK_WINDOW_TYPE_HINT_DESKTOP);

    gtk_widget_set_opacity(
        popup,
        drawer->dock->opacity_percent / 100.0);

    GtkWidget *fixed =
        gtk_fixed_new();

    gtk_container_add(
        GTK_CONTAINER(popup),
        fixed);

    dock_enable_desktop_drop_on_drawer(
        fixed,
        drawer);

    if (drawer->entries->len == 0) {
        GtkWidget *empty =
            gtk_label_new("Empty");

        gtk_widget_set_size_request(
            empty,
            DOCK_TILE_SIZE,
            DOCK_TILE_SIZE);

        gtk_widget_set_halign(
            empty,
            GTK_ALIGN_CENTER);

        gtk_widget_set_valign(
            empty,
            GTK_ALIGN_CENTER);

        gtk_fixed_put(
            GTK_FIXED(fixed),
            empty,
            0,
            0);
    } else {
        for (guint i = 0;
             i < drawer->entries->len;
             i++) {
            dock_drawer_popup_add_entry(
                drawer,
                GTK_FIXED(fixed),
                g_ptr_array_index(
                    drawer->entries,
                    i),
                i);
        }
    }

    guint tile_count =
        MAX((guint)1, drawer->entries->len);

    gtk_window_set_default_size(
        GTK_WINDOW(popup),
        (gint)tile_count * DOCK_TILE_SIZE,
        DOCK_TILE_SIZE);

    drawer->popup = popup;

    gtk_widget_show_all(popup);
}

static void
dock_drawer_destroy_popup(
    DockDrawer *drawer)
{
    if (drawer->drag_tick_id &&
        drawer->dock &&
        drawer->dock->box) {
        gtk_widget_remove_tick_callback(
            drawer->dock->box,
            drawer->drag_tick_id);
        drawer->drag_tick_id = 0;
    }

    dock_drawer_drag_proxy_destroy(drawer);

    drawer->drag_press_active = FALSE;
    drawer->dragging = FALSE;
    drawer->dropping = FALSE;
    drawer->drag_button = NULL;
    drawer->drag_entry = NULL;
    drawer->source_index = -1;
    drawer->target_index = -1;
    drawer->drag_target_drawer = NULL;
    drawer->drag_target_slot = -1;

    if (drawer->outside_poll_id) {
        g_source_remove(
            drawer->outside_poll_id);
        drawer->outside_poll_id = 0;
    }

    if (drawer->animation_tick_id &&
        drawer->popup) {
        gtk_widget_remove_tick_callback(
            drawer->popup,
            drawer->animation_tick_id);
        drawer->animation_tick_id = 0;
    }

    if (!drawer->popup)
        return;

    GtkWidget *popup =
        drawer->popup;

    drawer->popup = NULL;
    drawer->animation_closing = FALSE;
    drawer->animation_progress = 0.0;

    if (drawer->dock &&
        drawer->dock->x11) {
        dock_x11_close_group_for_ancestor(
            drawer->dock->x11,
            popup);
    }

    gtk_widget_destroy(popup);
}

static void
dock_drawer_close(DockDrawer *drawer)
{
    if (drawer->hover_open_id) {
        g_source_remove(drawer->hover_open_id);
        drawer->hover_open_id = 0;
    }

    if (drawer->hover_close_id) {
        g_source_remove(drawer->hover_close_id);
        drawer->hover_close_id = 0;
    }

    if (!drawer->popup ||
        drawer->animation_closing)
        return;

    if (drawer->drag_tick_id &&
        drawer->dock &&
        drawer->dock->box) {
        gtk_widget_remove_tick_callback(
            drawer->dock->box,
            drawer->drag_tick_id);
        drawer->drag_tick_id = 0;
    }

    dock_drawer_drag_proxy_destroy(drawer);

    drawer->drag_press_active = FALSE;
    drawer->dragging = FALSE;
    drawer->dropping = FALSE;
    drawer->drag_button = NULL;
    drawer->drag_entry = NULL;
    drawer->drag_target_drawer = NULL;
    drawer->drag_target_slot = -1;

    if (drawer->dock &&
        drawer->dock->x11) {
        dock_x11_close_group_for_ancestor(
            drawer->dock->x11,
            drawer->popup);
    }

    if (drawer->outside_poll_id) {
        g_source_remove(
            drawer->outside_poll_id);
        drawer->outside_poll_id = 0;
    }

    drawer->animation_closing = TRUE;
    drawer->animation_start_us = 0;
    drawer->animation_progress = 1.0;

    if (drawer->animation_tick_id == 0) {
        drawer->animation_tick_id =
            gtk_widget_add_tick_callback(
                drawer->popup,
                dock_drawer_animation_tick,
                drawer,
                NULL);
    }
}

static gboolean
dock_drawer_get_anchor(
    DockDrawer *drawer,
    gint *x,
    gint *y)
{
    GtkWidget *button =
        drawer->button;

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(button);

    if (!toplevel ||
        !GTK_IS_WINDOW(toplevel))
        return FALSE;

    gint button_x = 0;
    gint button_y = 0;

    if (!gtk_widget_translate_coordinates(
            button,
            toplevel,
            0,
            0,
            &button_x,
            &button_y))
        return FALSE;

    gint window_x = 0;
    gint window_y = 0;

    gtk_window_get_position(
        GTK_WINDOW(toplevel),
        &window_x,
        &window_y);

    *x = window_x + button_x;
    *y = window_y + button_y;

    return TRUE;
}

static void
dock_drawer_position_popup(
    DockDrawer *drawer,
    gdouble progress)
{
    if (!drawer->popup)
        return;

    gint anchor_x = 0;
    gint anchor_y = 0;

    if (!dock_drawer_get_anchor(
            drawer,
            &anchor_x,
            &anchor_y))
        return;

    guint tile_count =
        MAX((guint)1, drawer->entries->len);

    gint popup_width =
        (gint)tile_count *
        DOCK_TILE_SIZE;

    gboolean opens_to_right =
        !drawer->dock->on_right_side;

    gint popup_x =
        opens_to_right ?
        anchor_x + DOCK_TILE_SIZE :
        anchor_x - popup_width;

    gint popup_y =
        anchor_y;

    /*
     * Keep the Drawer on the same physical monitor as the Dock. GTK/X11
     * windows are allowed to extend beyond the monitor by default, which is
     * especially awkward for wide Drawers on small displays.
     */
    GdkDisplay *display =
        gtk_widget_get_display(
            drawer->button);

    if (display) {
        /*
         * Resolve the monitor from the Drawer anchor itself rather than
         * the Dock toplevel. This is more reliable with unusual multi-
         * monitor layouts where the toplevel may straddle monitor bounds.
         */
        GdkMonitor *monitor =
            gdk_display_get_monitor_at_point(
                display,
                anchor_x + DOCK_TILE_SIZE / 2,
                anchor_y + DOCK_TILE_SIZE / 2);

        if (!monitor)
            monitor =
                gdk_display_get_primary_monitor(
                    display);

        if (monitor) {
            GdkRectangle geometry;

            gdk_monitor_get_geometry(
                monitor,
                &geometry);

            if (popup_width <= geometry.width) {
                gint min_x =
                    geometry.x;
                gint max_x =
                    geometry.x +
                    geometry.width -
                    popup_width;

                popup_x =
                    CLAMP(
                        popup_x,
                        min_x,
                        max_x);
            } else {
                popup_x =
                    geometry.x;
            }

            gint popup_height =
                DOCK_TILE_SIZE;

            if (popup_height <= geometry.height) {
                gint min_y =
                    geometry.y;
                gint max_y =
                    geometry.y +
                    geometry.height -
                    popup_height;

                popup_y =
                    CLAMP(
                        popup_y,
                        min_y,
                        max_y);
            } else {
                popup_y =
                    geometry.y;
            }
        }
    }

    gtk_window_move(
        GTK_WINDOW(drawer->popup),
        popup_x,
        popup_y);

    GtkWidget *fixed =
        gtk_bin_get_child(
            GTK_BIN(drawer->popup));

    if (!fixed ||
        !GTK_IS_FIXED(fixed))
        return;

    GList *children =
        gtk_container_get_children(
            GTK_CONTAINER(fixed));

    gint index = 0;

    for (GList *iter = children;
         iter;
         iter = iter->next) {
        GtkWidget *child =
            GTK_WIDGET(iter->data);

        gint final_x =
            opens_to_right ?
            (gint)index * DOCK_TILE_SIZE :
            popup_width -
            DOCK_TILE_SIZE * (index + 1);

        gint start_x =
            opens_to_right ?
            0 :
            popup_width -
            DOCK_TILE_SIZE;

        gint x =
            (gint)round(
                start_x +
                (final_x - start_x) *
                progress);

        gtk_fixed_move(
            GTK_FIXED(fixed),
            child,
            x,
            0);

        index++;
    }

    g_list_free(children);
}

static gboolean
dock_drawer_animation_tick(
    GtkWidget *widget,
    GdkFrameClock *frame_clock,
    gpointer user_data)
{
    (void)widget;

    DockDrawer *drawer = user_data;

    if (!drawer->popup)
        return G_SOURCE_REMOVE;

    gint64 now_us =
        gdk_frame_clock_get_frame_time(
            frame_clock);

    if (drawer->animation_start_us == 0)
        drawer->animation_start_us = now_us;

    gdouble elapsed =
        (gdouble)(
            now_us -
            drawer->animation_start_us) /
        DRAWER_ANIMATION_USEC;

    elapsed =
        CLAMP(
            elapsed,
            0.0,
            1.0);

    if (drawer->animation_closing) {
        drawer->animation_progress =
            1.0 - elapsed;
    } else {
        drawer->animation_progress =
            elapsed;
    }

    gdouble eased;
    if (drawer->animation_closing) {
        eased =
            drawer->animation_progress *
            drawer->animation_progress *
            drawer->animation_progress;
    } else {
        gdouble inv =
            1.0 -
            drawer->animation_progress;

        eased =
            1.0 -
            inv * inv * inv;
    }

    dock_drawer_position_popup(
        drawer,
        eased);

    if (elapsed >= 1.0) {
        drawer->animation_tick_id = 0;

        if (drawer->animation_closing) {
            dock_drawer_destroy_popup(
                drawer);
            return G_SOURCE_REMOVE;
        }

        drawer->animation_start_us = 0;
        drawer->animation_progress = 1.0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static void
dock_drawer_open(DockDrawer *drawer)
{
    if (drawer->hover_open_id) {
        g_source_remove(drawer->hover_open_id);
        drawer->hover_open_id = 0;
    }

    if (drawer->hover_close_id) {
        g_source_remove(drawer->hover_close_id);
        drawer->hover_close_id = 0;
    }

    dock_drawer_rebuild_popup(drawer);

    if (!drawer->popup)
        return;

    drawer->animation_closing = FALSE;
    drawer->animation_start_us = 0;
    drawer->animation_progress = 0.0;

    /*
     * Keep the popup at its final width while the individual blocks slide
     * out from the Dock-facing edge.
     */
    dock_drawer_position_popup(
        drawer,
        0.0);

    gtk_widget_show_all(
        drawer->popup);

    if (drawer->animation_tick_id == 0) {
        drawer->animation_tick_id =
            gtk_widget_add_tick_callback(
                drawer->popup,
                dock_drawer_animation_tick,
                drawer,
                NULL);
    }

    if (drawer->outside_poll_id == 0) {
        drawer->outside_poll_id =
            g_timeout_add(
                25,
                dock_drawer_outside_poll,
                drawer);
    }
}

static gboolean
dock_drawer_point_inside(
    DockDrawer *drawer,
    gint root_x,
    gint root_y)
{
    if (!drawer->popup)
        return FALSE;

    GtkAllocation popup_allocation;
    gtk_widget_get_allocation(
        drawer->popup,
        &popup_allocation);

    gint popup_x = 0;
    gint popup_y = 0;

    gtk_window_get_position(
        GTK_WINDOW(drawer->popup),
        &popup_x,
        &popup_y);

    if (root_x >= popup_x &&
        root_x < popup_x + popup_allocation.width &&
        root_y >= popup_y &&
        root_y < popup_y + popup_allocation.height)
        return TRUE;

    gint anchor_x = 0;
    gint anchor_y = 0;

    if (!dock_drawer_get_anchor(
            drawer,
            &anchor_x,
            &anchor_y))
        return FALSE;

    GtkAllocation anchor_allocation;
    gtk_widget_get_allocation(
        drawer->button,
        &anchor_allocation);

    if (root_x >= anchor_x &&
        root_x < anchor_x + anchor_allocation.width &&
        root_y >= anchor_y &&
        root_y < anchor_y + anchor_allocation.height)
        return TRUE;

    if (drawer->dock &&
        drawer->dock->x11 &&
        drawer->popup &&
        dock_x11_group_popup_point_inside(
            drawer->dock->x11,
            drawer->popup,
            root_x,
            root_y))
        return TRUE;

    return FALSE;
}

static gboolean
dock_drawer_outside_poll(gpointer user_data)
{
    DockDrawer *drawer = user_data;

    if (!drawer->popup)
        return G_SOURCE_REMOVE;

    GdkDisplay *gdk_display =
        gtk_widget_get_display(
            drawer->popup);

    if (!GDK_IS_X11_DISPLAY(gdk_display))
        return G_SOURCE_CONTINUE;

    Display *display =
        gdk_x11_display_get_xdisplay(
            gdk_display);

    Window root =
        DefaultRootWindow(display);

    Window root_return = None;
    Window child_return = None;
    gint root_x = 0;
    gint root_y = 0;
    gint win_x = 0;
    gint win_y = 0;
    guint mask = 0;

    if (!XQueryPointer(
            display,
            root,
            &root_return,
            &child_return,
            &root_x,
            &root_y,
            &win_x,
            &win_y,
            &mask))
        return G_SOURCE_CONTINUE;

    if (drawer->dock->x11) {
        GtkWidget *container =
            gtk_bin_get_child(
                GTK_BIN(drawer->popup));

        if (container &&
            GTK_IS_CONTAINER(container)) {
            GList *children =
                gtk_container_get_children(
                    GTK_CONTAINER(container));

            for (GList *iter = children;
                 iter;
                 iter = iter->next) {
                GtkWidget *button =
                    GTK_WIDGET(iter->data);

                if (g_object_get_data(
                        G_OBJECT(button),
                        "dock-drawer-entry")) {
                    dock_x11_refresh_button(
                        drawer->dock->x11,
                        button);
                }
            }

            g_list_free(children);
        }
    }

    gboolean pointer_inside =
        dock_drawer_point_inside(
            drawer,
            root_x,
            root_y);

    if (pointer_inside) {
        if (drawer->hover_close_id) {
            g_source_remove(
                drawer->hover_close_id);
            drawer->hover_close_id = 0;
        }

        return G_SOURCE_CONTINUE;
    }

    /*
     * A Drawer drag intentionally leaves the popup. Keep the popup open
     * while the pointer is grabbed so the dragged icon keeps animating.
     */
    if (drawer->dragging)
        return G_SOURCE_CONTINUE;

    if ((mask & (
             Button1Mask |
             Button2Mask |
             Button3Mask |
             Button4Mask |
             Button5Mask)) != 0) {
        dock_drawer_close(drawer);
        return G_SOURCE_REMOVE;
    }

    /*
     * Pointer is outside both the Drawer tile and popup. Delay collapse
     * here instead of from popup LeaveNotify so clicks on child buttons
     * cannot race the close animation.
     */
    if (!drawer->dragging &&
        !drawer->animation_closing &&
        !drawer->hover_close_id) {
        drawer->hover_close_id =
            g_timeout_add(
                DRAWER_HOVER_CLOSE_DELAY_MS,
                dock_drawer_hover_close,
                drawer);
    }

    return G_SOURCE_CONTINUE;
}

static void
dock_drawer_toggle(DockDrawer *drawer)
{
    if (!drawer)
        return;

    if (drawer->hover_open_id) {
        g_source_remove(drawer->hover_open_id);
        drawer->hover_open_id = 0;
    }

    if (drawer->hover_close_id) {
        g_source_remove(drawer->hover_close_id);
        drawer->hover_close_id = 0;
    }

    if (drawer->popup)
        dock_drawer_close(drawer);
    else
        dock_drawer_open(drawer);
}

static gboolean
dock_drawer_hover_open(gpointer user_data)
{
    DockDrawer *drawer = user_data;

    drawer->hover_open_id = 0;

    if (!drawer->popup &&
        !drawer->animation_closing)
        dock_drawer_open(drawer);

    return G_SOURCE_REMOVE;
}

static gboolean
dock_drawer_hover_close(gpointer user_data)
{
    DockDrawer *drawer = user_data;

    drawer->hover_close_id = 0;

    if (drawer->popup &&
        !drawer->animation_closing)
        dock_drawer_close(drawer);

    return G_SOURCE_REMOVE;
}

static gboolean
dock_drawer_enter_notify(
    GtkWidget *widget,
    GdkEventCrossing *event,
    gpointer user_data)
{
    (void)widget;
    (void)event;

    DockDrawer *drawer = user_data;

    if (drawer->hover_close_id) {
        g_source_remove(drawer->hover_close_id);
        drawer->hover_close_id = 0;
    }

    if (!drawer->popup &&
        !drawer->animation_closing &&
        !drawer->hover_open_id) {
        drawer->hover_open_id =
            g_timeout_add(
                250,
                dock_drawer_hover_open,
                drawer);
    }

    return FALSE;
}

static gboolean
dock_drawer_leave_notify(
    GtkWidget *widget,
    GdkEventCrossing *event,
    gpointer user_data)
{
    (void)widget;

    DockDrawer *drawer = user_data;

    if (event->detail == GDK_NOTIFY_INFERIOR)
        return FALSE;

    if (drawer->hover_open_id) {
        g_source_remove(drawer->hover_open_id);
        drawer->hover_open_id = 0;
    }

    return FALSE;
}

static gboolean
dock_drawer_add_desktop_id(
    DockDrawer *drawer,
    const gchar *desktop_id)
{
    if (!desktop_id ||
        dock_drawer_has_desktop_id(
            drawer,
            desktop_id))
        return FALSE;

    DockLauncher *launcher =
        dock_launcher_new_from_desktop_id(
            desktop_id);

    if (!launcher)
        return FALSE;

    DockDrawerEntry *entry =
        g_new0(DockDrawerEntry, 1);

    entry->desktop_id =
        g_strdup(desktop_id);
    entry->launcher =
        launcher;

    g_ptr_array_add(
        drawer->entries,
        entry);

    if (drawer->popup)
        dock_drawer_open(drawer);

    return TRUE;
}

static void dock_remove_menu_item(
    GtkMenuItem *menu_item,
    gpointer user_data);

static gint
dock_find_item_index(const Dock *dock, const DockItem *item)
{
    for (guint i = 0; i < dock->items->len; i++) {
        if (g_ptr_array_index(dock->items, i) == item)
            return (gint)i;
    }

    return -1;
}

static gboolean
dock_has_desktop_id(const Dock *dock, const gchar *desktop_id)
{
    for (guint i = 0; i < dock->items->len; i++) {
        DockItem *item =
            g_ptr_array_index(dock->items, i);

        if (g_strcmp0(item->desktop_id, desktop_id) == 0)
            return TRUE;
    }

    return FALSE;
}

static void
dock_move_button(
    Dock *dock,
    GtkWidget *button,
    gdouble y)
{
    GtkWidget *parent = gtk_widget_get_parent(button);

    if (!parent || !GTK_IS_FIXED(parent))
        return;

    gtk_fixed_move(
        GTK_FIXED(parent),
        button,
        dock_get_items_x(dock),
        (gint)round(y));
}

static void
dock_apply_item_position(
    Dock *dock,
    DockItem *item)
{
    gint pixel_y = (gint)round(item->visual_y);

    if (pixel_y == item->applied_y)
        return;

    dock_move_button(
        dock,
        item->button,
        item->visual_y);
    item->applied_y = pixel_y;
}

static void
dock_reset_all_targets(Dock *dock)
{
    for (guint i = 0; i < dock->items->len; i++) {
        DockItem *item =
            g_ptr_array_index(dock->items, i);

        item->visual_y =
            i * DOCK_TILE_SIZE;
        item->target_y =
            item->visual_y;
        item->applied_y = -1;

        dock_apply_item_position(dock, item);
    }
}

static void
dock_set_dragging_state(GtkWidget *button, gboolean dragging)
{
    g_object_set_data(
        G_OBJECT(button),
        "dock-dragging",
        GINT_TO_POINTER(dragging));
}

static void
dock_set_neighbor_targets(
    Dock *dock,
    gint source_index,
    gint target_index)
{
    for (guint i = 0; i < dock->items->len; i++) {
        DockItem *item =
            g_ptr_array_index(dock->items, i);

        if ((gint)i == source_index)
            continue;

        gint slot = (gint)i;

        if (target_index > source_index &&
            slot > source_index &&
            slot <= target_index) {
            slot--;
        } else if (
            target_index < source_index &&
            slot >= target_index &&
            slot < source_index) {
            slot++;
        }

        item->target_y =
            slot * DOCK_TILE_SIZE;
    }
}

static void
dock_set_final_targets(Dock *dock)
{
    for (guint i = 0; i < dock->items->len; i++) {
        DockItem *item =
            g_ptr_array_index(dock->items, i);

        item->target_y =
            i * DOCK_TILE_SIZE;
    }
}

static gboolean
dock_neighbors_settled(const Dock *dock)
{
    for (guint i = 0; i < dock->items->len; i++) {
        DockItem *item =
            g_ptr_array_index(dock->items, i);

        if (item == dock->drag_item)
            continue;

        if (fabs(item->visual_y - item->target_y) > 0.25)
            return FALSE;
    }

    return TRUE;
}

static void
dock_finish_drop(Dock *dock)
{
    if (!dock->drag_item)
        return;

    DockItem *item = dock->drag_item;
    gint final_y =
        dock->target_index * DOCK_TILE_SIZE;

    item->visual_y = final_y;
    item->target_y = final_y;
    item->applied_y = -1;

    dock_apply_item_position(dock, item);

    dock_set_dragging_state(
        item->button,
        FALSE);

    dock_drag_proxy_destroy(dock);

    dock_drawer_clear_drop_states(dock);

    if (dock->x11)
        dock_x11_set_group_popup_suppressed(
            dock->x11,
            FALSE);

    dock_reset_drag_drawer_state(dock);

    dock->press_group_popup_state = 0;
    dock->drag_item = NULL;
    dock->drag_button = NULL;
    dock->dragging = FALSE;
    dock->dropping = FALSE;
    dock->source_index = -1;
    dock->target_index = -1;
    dock->drag_target_y = 0.0;
    dock->last_frame_us = 0;
}

static gboolean
dock_drag_tick(
    GtkWidget *widget,
    GdkFrameClock *frame_clock,
    gpointer user_data)
{
    (void)widget;

    Dock *dock = user_data;

    if (!dock->drag_item)
        return G_SOURCE_REMOVE;

    gint64 now_us =
        gdk_frame_clock_get_frame_time(frame_clock);

    if (dock->last_frame_us == 0)
        dock->last_frame_us = now_us;

    gdouble dt =
        (gdouble)(now_us - dock->last_frame_us) /
        1000000.0;

    dock->last_frame_us = now_us;
    dt = CLAMP(dt, 0.0, 0.05);

    gdouble neighbor_blend =
        1.0 - exp(-DRAG_NEIGHBOR_RATE * dt);

    gdouble drag_blend =
        1.0 - exp(-DRAG_FOLLOW_RATE * dt);

    DockItem *drag_item =
        dock->drag_item;

    if (dock->drag_proxy &&
        dock->drag_proxy_visible) {
        gdouble proxy_blend =
            1.0 - exp(-DRAG_FOLLOW_RATE * dt);

        dock->drag_proxy_x +=
            (dock->drag_proxy_target_x -
             dock->drag_proxy_x) *
            proxy_blend;

        dock->drag_proxy_y +=
            (dock->drag_proxy_target_y -
             dock->drag_proxy_y) *
            proxy_blend;

        gtk_window_move(
            GTK_WINDOW(dock->drag_proxy),
            (gint)round(dock->drag_proxy_x),
            (gint)round(dock->drag_proxy_y));
    }

    if (dock->dragging && !dock->dropping) {
        gdouble delta =
            dock->drag_target_y -
            drag_item->visual_y;

        if (fabs(delta) <= 0.1)
            drag_item->visual_y =
                dock->drag_target_y;
        else
            drag_item->visual_y +=
                delta * drag_blend;

        dock_apply_item_position(dock, drag_item);
    }

    for (guint i = 0; i < dock->items->len; i++) {
        DockItem *item =
            g_ptr_array_index(dock->items, i);

        if (item == drag_item)
            continue;

        gdouble delta =
            item->target_y -
            item->visual_y;

        if (fabs(delta) <= 0.1)
            item->visual_y =
                item->target_y;
        else
            item->visual_y +=
                delta * neighbor_blend;

        dock_apply_item_position(dock, item);
    }

    if (dock->dropping) {
        gdouble target_y =
            dock->target_index * DOCK_TILE_SIZE;

        gdouble delta =
            target_y -
            drag_item->visual_y;

        if (fabs(delta) <= 0.1)
            drag_item->visual_y = target_y;
        else
            drag_item->visual_y +=
                delta * drag_blend;

        dock_apply_item_position(dock, drag_item);

        if (dock_neighbors_settled(dock) &&
            fabs(drag_item->visual_y - target_y) <= 0.1) {
            dock_finish_drop(dock);
            dock->tick_id = 0;
            return G_SOURCE_REMOVE;
        }
    }

    return G_SOURCE_CONTINUE;
}

static void
dock_ensure_tick(Dock *dock)
{
    if (dock->tick_id)
        return;

    dock->last_frame_us = 0;

    dock->tick_id =
        gtk_widget_add_tick_callback(
            dock->box,
            dock_drag_tick,
            dock,
            NULL);
}

static void
dock_save(Dock *dock)
{
    gsize item_count =
        dock->items->len;

    gchar **items =
        g_new0(
            gchar *,
            item_count + 1);

    DockConfigDrawer *drawers =
        g_new0(
            DockConfigDrawer,
            item_count);

    gsize drawer_count = 0;

    for (gsize i = 0; i < item_count; i++) {
        DockItem *item =
            g_ptr_array_index(
                dock->items,
                i);

        if (!item->is_drawer) {
            items[i] =
                g_strdup_printf(
                    "app:%s",
                    item->desktop_id);
            continue;
        }

        items[i] =
            g_strdup_printf(
                "drawer:%zu",
                drawer_count);

        DockConfigDrawer *config_drawer =
            &drawers[drawer_count++];

        config_drawer->name =
            g_strdup(
                item->drawer->name);

        config_drawer->application_count =
            item->drawer->entries->len;

        if (config_drawer->application_count > 0) {
            config_drawer->applications =
                g_new0(
                    gchar *,
                    config_drawer->application_count + 1);

            for (gsize j = 0;
                 j < config_drawer->application_count;
                 j++) {
                DockDrawerEntry *entry =
                    g_ptr_array_index(
                        item->drawer->entries,
                        j);

                config_drawer->applications[j] =
                    g_strdup(
                        entry->desktop_id);
            }
        }
    }

    const gchar *monitor_manufacturer = NULL;
    const gchar *monitor_model = NULL;

    GdkDisplay *display =
        gtk_widget_get_display(dock->box);

    if (display) {
        gint monitor_count =
            gdk_display_get_n_monitors(display);

        if (dock->monitor_index >= 0 &&
            dock->monitor_index < monitor_count) {
            GdkMonitor *monitor =
                gdk_display_get_monitor(
                    display,
                    dock->monitor_index);

            if (monitor) {
                monitor_manufacturer =
                    gdk_monitor_get_manufacturer(
                        monitor);
                monitor_model =
                    gdk_monitor_get_model(
                        monitor);
            }
        }
    }

    GError *error = NULL;

    if (!dock_config_save_state(
            (const gchar *const *)items,
            item_count,
            drawers,
            drawer_count,
            dock->on_right_side,
            (gint)dock->position_mode,
            dock->monitor_index,
            monitor_manufacturer,
            monitor_model,
            dock->icon_size,
            dock->opacity_percent,
            dock->show_window_indicator,
            dock->show_hide_handle,
            dock->shortcut_only,
            dock->minimized_window_icons,
            dock->minimized_title_labels,
            dock->minimized_group_drawer,
            dock->minimized_all_workspaces,
            dock->bitmap_background_enabled,
            (gint)dock->bitmap_background_mode,
            dock->bitmap_background_path,
            &error)) {
        g_warning(
            "Unable to save Dock configuration: %s",
            error ? error->message : "unknown error");
        g_clear_error(&error);
    }

    dock_config_free_drawers(
        drawers,
        drawer_count);

    g_strfreev(items);
}

void
dock_save_configuration(
    Dock *dock)
{
    g_return_if_fail(dock != NULL);
    dock_save(dock);
}

static void
dock_resize_to_items(Dock *dock)
{
    gint width = dock_get_window_width(dock);
    gint height = dock_get_window_height(dock);

    gtk_widget_set_size_request(
        dock->box,
        width,
        height);

    if (dock->hide_handle_button) {
        gtk_widget_set_size_request(
            dock->hide_handle_button,
            dock->show_hide_handle ?
                DOCK_TILE_SIZE : -1,
            dock->show_hide_handle ?
                MAX(DOCK_TILE_SIZE / 2, 1) : -1);
        gtk_fixed_move(
            GTK_FIXED(dock->box),
            dock->hide_handle_button,
            0,
            dock_get_handle_y(dock));
    }

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(dock->box);

    if (toplevel &&
        GTK_IS_WINDOW(toplevel)) {
        gtk_window_resize(
            GTK_WINDOW(toplevel),
            width,
            height);
        gtk_widget_queue_resize(toplevel);
    } else {
        gtk_widget_queue_resize(dock->box);
    }
}

static void
dock_compact_items(Dock *dock)
{
    dock_set_final_targets(dock);

    for (guint i = 0; i < dock->items->len; i++) {
        DockItem *item =
            g_ptr_array_index(dock->items, i);

        item->visual_y =
            item->target_y;
        item->applied_y = -1;

        dock_apply_item_position(dock, item);
    }

    dock_resize_to_items(dock);
}

typedef enum {
    DOCK_APP_COL_ICON,
    DOCK_APP_COL_MARKUP,
    DOCK_APP_COL_DESKTOP_ID,
    DOCK_APP_COL_SEARCH,
    DOCK_APP_COL_COUNT
} DockAppColumn;

typedef struct {
    Dock *dock;
    DockDrawer *drawer;
    GtkWidget *dialog;
    GtkWidget *search_entry;
    GtkWidget *tree_view;
    GtkWidget *add_button;
    GtkListStore *store;
    GtkTreeModelFilter *filter_model;
    gchar *search_text;
} DockAppChooser;

static GtkTargetEntry dock_chooser_drag_targets[] = {
    {
        "text/uri-list",
        0,
        0
    }
};

static void
dock_app_chooser_drag_data_get(
    GtkWidget *widget,
    GdkDragContext *context,
    GtkSelectionData *selection_data,
    guint info,
    guint time,
    gpointer user_data)
{
    (void)widget;
    (void)context;
    (void)info;
    (void)time;

    DockAppChooser *chooser = user_data;

    if (!chooser ||
        !chooser->tree_view ||
        !chooser->filter_model ||
        !chooser->store)
        return;

    GtkTreeSelection *selection =
        gtk_tree_view_get_selection(
            GTK_TREE_VIEW(chooser->tree_view));

    GtkTreeIter filter_iter;

    if (!gtk_tree_selection_get_selected(
            selection,
            NULL,
            &filter_iter))
        return;

    GtkTreeIter iter;

    gtk_tree_model_filter_convert_iter_to_child_iter(
        chooser->filter_model,
        &iter,
        &filter_iter);

    gchar *desktop_id = NULL;

    gtk_tree_model_get(
        GTK_TREE_MODEL(chooser->store),
        &iter,
        DOCK_APP_COL_DESKTOP_ID,
        &desktop_id,
        -1);

    if (!desktop_id)
        return;

    GDesktopAppInfo *app =
        g_desktop_app_info_new(desktop_id);

    if (app) {
        const gchar *filename =
            g_desktop_app_info_get_filename(app);

        if (filename && *filename) {
            gchar *uri =
                g_filename_to_uri(
                    filename,
                    NULL,
                    NULL);

            if (uri) {
                gchar *uris[] = {
                    uri,
                    NULL
                };

                gtk_selection_data_set_uris(
                    selection_data,
                    uris);

                g_free(uri);
            }
        }

        g_object_unref(app);
    }

    g_free(desktop_id);
}

static gint
compare_app_names(
    gconstpointer a,
    gconstpointer b)
{
    const gchar *name_a =
        g_app_info_get_display_name(
            G_APP_INFO(a));

    const gchar *name_b =
        g_app_info_get_display_name(
            G_APP_INFO(b));

    return g_utf8_collate(
        name_a ? name_a : "",
        name_b ? name_b : "");
}

static gboolean
dock_app_chooser_filter_visible(
    GtkTreeModel *model,
    GtkTreeIter *iter,
    gpointer user_data)
{
    DockAppChooser *chooser = user_data;

    if (!chooser->search_text ||
        !*chooser->search_text)
        return TRUE;

    gchar *search = NULL;

    gtk_tree_model_get(
        model,
        iter,
        DOCK_APP_COL_SEARCH,
        &search,
        -1);

    gboolean visible =
        search &&
        g_strrstr(
            search,
            chooser->search_text) != NULL;

    g_free(search);
    return visible;
}

static void
dock_app_chooser_update_add_button(
    DockAppChooser *chooser)
{
    GtkTreeSelection *selection =
        gtk_tree_view_get_selection(
            GTK_TREE_VIEW(chooser->tree_view));

    GtkTreeIter iter;

    gtk_widget_set_sensitive(
        chooser->add_button,
        gtk_tree_selection_get_selected(
            selection,
            NULL,
            &iter));
}

static gboolean
dock_app_chooser_add_selected(
    DockAppChooser *chooser)
{
    GtkTreeSelection *selection =
        gtk_tree_view_get_selection(
            GTK_TREE_VIEW(chooser->tree_view));

    GtkTreeIter filter_iter;

    if (!gtk_tree_selection_get_selected(
            selection,
            NULL,
            &filter_iter))
        return FALSE;

    GtkTreeIter iter;

    gtk_tree_model_filter_convert_iter_to_child_iter(
        chooser->filter_model,
        &iter,
        &filter_iter);

    gchar *desktop_id = NULL;

    gtk_tree_model_get(
        GTK_TREE_MODEL(chooser->store),
        &iter,
        DOCK_APP_COL_DESKTOP_ID,
        &desktop_id,
        -1);

    if (!desktop_id)
        return FALSE;

    gboolean added =
        chooser->drawer ?
        dock_drawer_add_desktop_id(
            chooser->drawer,
            desktop_id) :
        dock_add_desktop_id(
            chooser->dock,
            desktop_id);

    g_free(desktop_id);

    if (!added)
        return FALSE;

    gtk_list_store_remove(
        chooser->store,
        &iter);

    dock_save(
        chooser->drawer ?
        chooser->drawer->dock :
        chooser->dock);

    dock_app_chooser_update_add_button(
        chooser);

    return TRUE;
}

static void
dock_app_chooser_search_changed(
    GtkSearchEntry *entry,
    gpointer user_data)
{
    DockAppChooser *chooser = user_data;

    gchar *search =
        g_utf8_casefold(
            gtk_entry_get_text(
                GTK_ENTRY(entry)),
            -1);

    g_free(chooser->search_text);
    chooser->search_text = search;

    gtk_tree_model_filter_refilter(
        chooser->filter_model);

    GtkTreeSelection *selection =
        gtk_tree_view_get_selection(
            GTK_TREE_VIEW(chooser->tree_view));

    GtkTreeIter first;

    if (gtk_tree_model_get_iter_first(
            GTK_TREE_MODEL(chooser->filter_model),
            &first)) {
        GtkTreePath *path =
            gtk_tree_model_get_path(
                GTK_TREE_MODEL(chooser->filter_model),
                &first);

        gtk_tree_selection_unselect_all(
            selection);

        gtk_tree_selection_select_path(
            selection,
            path);

        gtk_tree_path_free(path);
    } else {
        gtk_tree_selection_unselect_all(selection);
    }

    dock_app_chooser_update_add_button(chooser);
}

static void
dock_app_chooser_search_activated(
    GtkSearchEntry *entry,
    gpointer user_data)
{
    (void)entry;
    dock_app_chooser_add_selected(user_data);
}

static void
dock_app_chooser_selection_changed(
    GtkTreeSelection *selection,
    gpointer user_data)
{
    (void)selection;
    dock_app_chooser_update_add_button(user_data);
}

static void
dock_app_chooser_row_activated(
    GtkTreeView *tree_view,
    GtkTreePath *path,
    GtkTreeViewColumn *column,
    gpointer user_data)
{
    (void)tree_view;
    (void)path;
    (void)column;
    dock_app_chooser_add_selected(user_data);
}

static void
dock_app_chooser_response(
    GtkDialog *dialog,
    gint response_id,
    gpointer user_data)
{
    if (response_id == GTK_RESPONSE_ACCEPT) {
        dock_app_chooser_add_selected(user_data);
        return;
    }

    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
dock_app_chooser_destroy(
    GtkWidget *widget,
    gpointer user_data)
{
    DockAppChooser *chooser = user_data;

    (void)widget;

    g_clear_pointer(
        &chooser->search_text,
        g_free);

    g_clear_object(
        &chooser->filter_model);

    g_clear_object(
        &chooser->store);

    g_free(chooser);
}

static void
dock_add_application_dialog_populate(
    DockAppChooser *chooser)
{
    GList *apps =
        g_list_sort(
            g_app_info_get_all(),
            compare_app_names);

    for (GList *iter = apps;
         iter;
         iter = iter->next) {
        GAppInfo *app =
            G_APP_INFO(iter->data);

        if (!G_IS_DESKTOP_APP_INFO(app) ||
            !g_app_info_should_show(app))
            continue;

        const gchar *filename =
            g_desktop_app_info_get_filename(
                G_DESKTOP_APP_INFO(app));

        if (!filename)
            continue;

        gchar *desktop_id =
            g_path_get_basename(filename);

        gboolean already_present =
            chooser->drawer ?
            dock_drawer_has_desktop_id(
                chooser->drawer,
                desktop_id) :
            dock_has_desktop_id(
                chooser->dock,
                desktop_id);

        if (already_present) {
            g_free(desktop_id);
            continue;
        }

        const gchar *name =
            g_app_info_get_display_name(app);

        if (!name || !*name)
            name = desktop_id;

        const gchar *description =
            g_app_info_get_description(app);

        gchar *escaped_name =
            g_markup_escape_text(name, -1);

        gchar *markup = NULL;

        if (description && *description) {
            gchar *escaped_description =
                g_markup_escape_text(
                    description,
                    -1);

            markup =
                g_strdup_printf(
                    "<b>%s</b>\n<small>%s</small>",
                    escaped_name,
                    escaped_description);

            g_free(escaped_description);
        } else {
            markup =
                g_strdup_printf(
                    "<b>%s</b>",
                    escaped_name);
        }

        g_free(escaped_name);

        gchar *search =
            g_utf8_casefold(name, -1);

        if (description && *description) {
            gchar *description_search =
                g_utf8_casefold(
                    description,
                    -1);

            gchar *combined =
                g_strconcat(
                    search,
                    " ",
                    description_search,
                    NULL);

            g_free(search);
            g_free(description_search);
            search = combined;
        }

        GtkTreeIter iter;

        gtk_list_store_append(
            chooser->store,
            &iter);

        gtk_list_store_set(
            chooser->store,
            &iter,
            DOCK_APP_COL_ICON,
            g_app_info_get_icon(app),
            DOCK_APP_COL_MARKUP,
            markup,
            DOCK_APP_COL_DESKTOP_ID,
            desktop_id,
            DOCK_APP_COL_SEARCH,
            search,
            -1);

        g_free(markup);
        g_free(search);
        g_free(desktop_id);
    }

    g_list_free_full(
        apps,
        g_object_unref);
}

static void
dock_show_application_chooser(
    Dock *dock,
    DockDrawer *drawer)
{
    if (!dock ||
        !dock->box ||
        (drawer && drawer->dock != dock))
        return;

    DockAppChooser *chooser =
        g_new0(
            DockAppChooser,
            1);

    chooser->dock = dock;
    chooser->drawer = drawer;

    GtkWidget *parent =
        gtk_widget_get_toplevel(dock->box);

    chooser->dialog =
        gtk_dialog_new_with_buttons(
            drawer ?
            "Add Application to Drawer" :
            "Add Application",
            parent && GTK_IS_WINDOW(parent) ?
            GTK_WINDOW(parent) :
            NULL,
            GTK_DIALOG_MODAL,
            "_Cancel",
            GTK_RESPONSE_CANCEL,
            "_Add",
            GTK_RESPONSE_ACCEPT,
            NULL);

    gtk_window_set_default_size(
        GTK_WINDOW(chooser->dialog),
        560,
        520);

    GtkWidget *content =
        gtk_dialog_get_content_area(
            GTK_DIALOG(chooser->dialog));

    gtk_container_set_border_width(
        GTK_CONTAINER(content),
        10);

    GtkWidget *label =
        gtk_label_new(
            drawer ?
            "Choose an application to add to this Drawer:" :
            "Choose an application to add to the Dock:");

    gtk_widget_set_halign(
        label,
        GTK_ALIGN_START);

    gtk_box_pack_start(
        GTK_BOX(content),
        label,
        FALSE,
        FALSE,
        0);

    chooser->search_entry =
        gtk_search_entry_new();

    gtk_entry_set_placeholder_text(
        GTK_ENTRY(chooser->search_entry),
        "Search applications...");

    gtk_box_pack_start(
        GTK_BOX(content),
        chooser->search_entry,
        FALSE,
        FALSE,
        0);

    chooser->store =
        gtk_list_store_new(
            DOCK_APP_COL_COUNT,
            G_TYPE_ICON,
            G_TYPE_STRING,
            G_TYPE_STRING,
            G_TYPE_STRING);

    chooser->filter_model =
        GTK_TREE_MODEL_FILTER(
            gtk_tree_model_filter_new(
                GTK_TREE_MODEL(chooser->store),
                NULL));

    gtk_tree_model_filter_set_visible_func(
        chooser->filter_model,
        dock_app_chooser_filter_visible,
        chooser,
        NULL);

    chooser->tree_view =
        gtk_tree_view_new_with_model(
            GTK_TREE_MODEL(chooser->filter_model));

    gtk_drag_source_set(
        chooser->tree_view,
        GDK_BUTTON1_MASK,
        dock_chooser_drag_targets,
        G_N_ELEMENTS(
            dock_chooser_drag_targets),
        GDK_ACTION_COPY);

    g_object_set_data(
        G_OBJECT(chooser->tree_view),
        "dock-app-chooser-source",
        GINT_TO_POINTER(TRUE));

    g_signal_connect(
        chooser->tree_view,
        "drag-data-get",
        G_CALLBACK(
            dock_app_chooser_drag_data_get),
        chooser);

    gtk_tree_view_set_headers_visible(
        GTK_TREE_VIEW(chooser->tree_view),
        FALSE);

    gtk_tree_view_set_enable_search(
        GTK_TREE_VIEW(chooser->tree_view),
        FALSE);

    gtk_widget_set_vexpand(
        chooser->tree_view,
        TRUE);

    GtkCellRenderer *icon_renderer =
        gtk_cell_renderer_pixbuf_new();

    g_object_set(
        icon_renderer,
        "xpad", 8,
        "ypad", 5,
        NULL);

    GtkTreeViewColumn *icon_column =
        gtk_tree_view_column_new_with_attributes(
            "",
            icon_renderer,
            "gicon",
            DOCK_APP_COL_ICON,
            NULL);

    gtk_tree_view_append_column(
        GTK_TREE_VIEW(chooser->tree_view),
        icon_column);

    GtkCellRenderer *text_renderer =
        gtk_cell_renderer_text_new();

    g_object_set(
        text_renderer,
        "xpad", 6,
        "ypad", 5,
        "ellipsize",
        PANGO_ELLIPSIZE_END,
        NULL);

    GtkTreeViewColumn *text_column =
        gtk_tree_view_column_new_with_attributes(
            "",
            text_renderer,
            "markup",
            DOCK_APP_COL_MARKUP,
            NULL);

    gtk_tree_view_column_set_expand(
        text_column,
        TRUE);

    gtk_tree_view_append_column(
        GTK_TREE_VIEW(chooser->tree_view),
        text_column);

    GtkWidget *scrolled =
        gtk_scrolled_window_new(
            NULL,
            NULL);

    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scrolled),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC);

    gtk_widget_set_vexpand(
        scrolled,
        TRUE);

    gtk_container_add(
        GTK_CONTAINER(scrolled),
        chooser->tree_view);

    gtk_box_pack_start(
        GTK_BOX(content),
        scrolled,
        TRUE,
        TRUE,
        0);

    chooser->add_button =
        gtk_dialog_get_widget_for_response(
            GTK_DIALOG(chooser->dialog),
            GTK_RESPONSE_ACCEPT);

    gtk_button_set_image(
        GTK_BUTTON(chooser->add_button),
        gtk_image_new_from_icon_name(
            "list-add",
            GTK_ICON_SIZE_BUTTON));

    gtk_button_set_always_show_image(
        GTK_BUTTON(chooser->add_button),
        TRUE);

    gtk_dialog_set_default_response(
        GTK_DIALOG(chooser->dialog),
        GTK_RESPONSE_ACCEPT);

    GtkTreeSelection *selection =
        gtk_tree_view_get_selection(
            GTK_TREE_VIEW(chooser->tree_view));

    gtk_tree_selection_set_mode(
        selection,
        GTK_SELECTION_SINGLE);

    g_signal_connect(
        selection,
        "changed",
        G_CALLBACK(
            dock_app_chooser_selection_changed),
        chooser);

    g_signal_connect(
        chooser->search_entry,
        "search-changed",
        G_CALLBACK(
            dock_app_chooser_search_changed),
        chooser);

    g_signal_connect(
        chooser->search_entry,
        "activate",
        G_CALLBACK(
            dock_app_chooser_search_activated),
        chooser);

    g_signal_connect(
        chooser->tree_view,
        "row-activated",
        G_CALLBACK(
            dock_app_chooser_row_activated),
        chooser);

    g_signal_connect(
        chooser->dialog,
        "response",
        G_CALLBACK(
            dock_app_chooser_response),
        chooser);

    g_signal_connect(
        chooser->dialog,
        "destroy",
        G_CALLBACK(
            dock_app_chooser_destroy),
        chooser);

    dock_add_application_dialog_populate(
        chooser);

    gtk_widget_show_all(
        chooser->dialog);

    gtk_widget_grab_focus(
        chooser->search_entry);

    dock_app_chooser_update_add_button(
        chooser);
}

static void
dock_add_application_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    (void)menu_item;

    dock_show_application_chooser(
        user_data,
        NULL);
}

static void
dock_add_drawer_application_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    (void)menu_item;

    DockDrawer *drawer =
        user_data;

    if (drawer)
        dock_show_application_chooser(
            drawer->dock,
            drawer);
}

static gboolean
dock_move_to_drawer(
    Dock *dock,
    DockItem *item,
    DockDrawer *drawer)
{
    if (!dock ||
        !item ||
        item->is_drawer ||
        !drawer ||
        !item->launcher)
        return FALSE;

    if (dock_count_application_items(dock) <= 1)
        return FALSE;

    if (dock_drawer_has_desktop_id(
            drawer,
            item->desktop_id))
        return FALSE;

    gboolean was_open =
        drawer->popup != NULL;

    if (!dock_drawer_add_desktop_id(
            drawer,
            item->desktop_id))
        return FALSE;

    if (dock->x11)
        dock_x11_close_group_for_ancestor(
            dock->x11,
            item->button);

    gint index =
        dock_find_item_index(
            dock,
            item);

    if (index < 0)
        return FALSE;

    DockItem *removed =
        g_ptr_array_steal_index(
            dock->items,
            (guint)index);

    gtk_widget_destroy(removed->button);
    dock_item_free(removed);

    dock_compact_items(dock);

    if (!was_open)
        dock_drawer_open(drawer);

    dock_save(dock);

    return TRUE;
}

static void
dock_move_to_drawer_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    Dock *dock = user_data;
    DockItem *item =
        g_object_get_data(
            G_OBJECT(menu_item),
            "dock-move-item");

    DockDrawer *drawer =
        g_object_get_data(
            G_OBJECT(menu_item),
            "dock-move-drawer");

    if (!dock_move_to_drawer(
            dock,
            item,
            drawer)) {
        g_warning(
            "Unable to move Dock application to Drawer");
    }
}

static void
dock_build_move_to_drawer_submenu(
    GtkMenu *submenu,
    Dock *dock,
    DockItem *item)
{
    guint added = 0;

    for (guint i = 0;
         i < dock->items->len;
         i++) {
        DockItem *target =
            g_ptr_array_index(
                dock->items,
                i);

        if (!target->is_drawer ||
            !target->drawer ||
            dock_drawer_has_desktop_id(
                target->drawer,
                item->desktop_id))
            continue;

        gchar *label =
            g_strdup(
                target->drawer->name ?
                target->drawer->name :
                "Drawer");

        GtkWidget *menu_item =
            gtk_menu_item_new_with_label(
                label);

        g_free(label);

        g_object_set_data(
            G_OBJECT(menu_item),
            "dock-move-item",
            item);

        g_object_set_data(
            G_OBJECT(menu_item),
            "dock-move-drawer",
            target->drawer);

        g_signal_connect(
            menu_item,
            "activate",
            G_CALLBACK(
                dock_move_to_drawer_menu_item_activated),
            dock);

        gtk_menu_shell_append(
            GTK_MENU_SHELL(submenu),
            menu_item);

        added++;
    }

    if (added == 0) {
        GtkWidget *empty =
            gtk_menu_item_new_with_label(
                "No available Drawers");

        gtk_widget_set_sensitive(
            empty,
            FALSE);

        gtk_menu_shell_append(
            GTK_MENU_SHELL(submenu),
            empty);
    }
}

static void
dock_move_to_dock_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    Dock *dock = user_data;

    DockDrawer *drawer =
        g_object_get_data(
            G_OBJECT(menu_item),
            "dock-move-drawer");

    DockDrawerEntry *entry =
        g_object_get_data(
            G_OBJECT(menu_item),
            "dock-move-entry");

    if (!drawer ||
        !entry ||
        !entry->desktop_id ||
        dock_has_desktop_id(
            dock,
            entry->desktop_id))
        return;

    if (!dock_add_desktop_id(
            dock,
            entry->desktop_id))
        return;

    guint index = G_MAXUINT;

    for (guint i = 0;
         i < drawer->entries->len;
         i++) {
        if (g_ptr_array_index(
                drawer->entries,
                i) == entry) {
            index = i;
            break;
        }
    }

    if (index == G_MAXUINT)
        return;

    g_ptr_array_remove_index(
        drawer->entries,
        index);

    if (drawer->popup)
        dock_drawer_open(drawer);

    dock_save(dock);
}

static void
dock_launch_menu_item(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    (void)menu_item;

    DockItem *item = user_data;

    GError *error = NULL;

    if (!dock_launcher_launch(
            item->launcher,
            item->button,
            gtk_get_current_event_time(),
            &error)) {
        g_warning(
            "Unable to launch Dock application '%s': %s",
            dock_launcher_get_name(item->launcher),
            error ? error->message : "unknown error");
        g_clear_error(&error);
    } else {
        dock_icon_button_set_launching(
            item->button,
            TRUE);
    }
}


static guint
dock_count_application_items(
    const Dock *dock)
{
    guint count = 0;

    for (guint i = 0;
         i < dock->items->len;
         i++) {
        DockItem *item =
            g_ptr_array_index(
                dock->items,
                i);

        if (!item->is_drawer)
            count++;
    }

    return count;
}

static void
dock_remove_menu_item(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    (void)menu_item;

    Dock *dock = user_data;

    DockItem *item =
        g_object_get_data(
            G_OBJECT(menu_item),
            "dock-item");

    if (!item ||
        dock->dragging ||
        dock->dropping)
        return;

    if (!item->is_drawer &&
        dock_count_application_items(dock) <= 1)
        return;

    if (item->is_drawer &&
        dock->items->len <= 1)
        return;

    if (item->is_drawer) {
        /*
         * Do not silently destroy the applications stored in a Drawer.
         * Crepido detaches Drawer contents rather than treating them
         * as disposable state. In this standalone Dock, the closest
         * observable equivalent is to return unique applications to the
         * main Dock before removing the Drawer itself.
         */
        DockDrawer *drawer =
            item->drawer;

        GPtrArray *desktop_ids =
            g_ptr_array_new_with_free_func(g_free);

        for (guint i = 0;
             i < drawer->entries->len;
             i++) {
            DockDrawerEntry *entry =
                g_ptr_array_index(
                    drawer->entries,
                    i);

            if (!dock_has_desktop_id(
                    dock,
                    entry->desktop_id)) {
                g_ptr_array_add(
                    desktop_ids,
                    g_strdup(
                        entry->desktop_id));
            }
        }

        if (drawer->popup)
            dock_drawer_close(drawer);

        for (guint i = 0;
             i < desktop_ids->len;
             i++) {
            const gchar *desktop_id =
                g_ptr_array_index(
                    desktop_ids,
                    i);

            dock_add_desktop_id(
                dock,
                desktop_id);
        }

        g_ptr_array_free(
            desktop_ids,
            TRUE);
    }

    gint index =
        dock_find_item_index(
            dock,
            item);

    if (index < 0)
        return;

    DockItem *removed =
        g_ptr_array_steal_index(
            dock->items,
            (guint)index);

    gtk_widget_destroy(removed->button);
    dock_item_free(removed);

    dock_compact_items(dock);
    dock_save(dock);
}


gboolean
dock_add_drawer(
    Dock *dock,
    const gchar *name,
    const gchar *const *desktop_ids,
    gsize desktop_id_count)
{
    DockDrawer *drawer =
        g_new0(DockDrawer, 1);

    drawer->dock = dock;
    drawer->name =
        g_strdup(name ? name : "Drawer");
    drawer->entries =
        g_ptr_array_new_with_free_func(
            dock_drawer_entry_free);

    DockItem *item =
        g_new0(DockItem, 1);

    item->is_drawer = TRUE;
    item->drawer = drawer;
    item->button =
        gtk_button_new();

    gtk_style_context_add_class(
        gtk_widget_get_style_context(item->button),
        "crepido-bitmap-block");

    GtkWidget *image =
        gtk_image_new_from_icon_name(
            "folder",
            GTK_ICON_SIZE_DIALOG);

    gtk_image_set_pixel_size(
        GTK_IMAGE(image),
        dock->icon_size);

    g_object_set_data(
        G_OBJECT(item->button),
        "dock-icon-image",
        image);

    gtk_container_add(
        GTK_CONTAINER(item->button),
        image);

    gtk_widget_set_size_request(
        item->button,
        DOCK_TILE_SIZE,
        DOCK_TILE_SIZE);

    drawer->button =
        item->button;

    g_signal_connect_after(
        item->button,
        "draw",
        G_CALLBACK(dock_drawer_button_draw),
        NULL);

    item->visual_y =
        dock->items->len * DOCK_TILE_SIZE;
    item->target_y =
        item->visual_y;
    item->applied_y = -1;

    gtk_fixed_put(
        GTK_FIXED(dock->box),
        item->button,
        dock_get_items_x(dock),
        (gint)item->visual_y);

    g_object_set_data(
        G_OBJECT(item->button),
        "dock-item",
        item);

    g_signal_connect_after(
        item->button,
        "draw",
        G_CALLBACK(dock_insertion_indicator_draw),
        NULL);

    dock_set_dragging_state(
        item->button,
        FALSE);

    gtk_widget_set_tooltip_text(
        item->button,
        drawer->name);

    gtk_widget_add_events(
        item->button,
        GDK_BUTTON_PRESS_MASK |
        GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK |
        GDK_ENTER_NOTIFY_MASK |
        GDK_LEAVE_NOTIFY_MASK);

    g_signal_connect(
        item->button,
        "enter-notify-event",
        G_CALLBACK(dock_drawer_enter_notify),
        drawer);

    g_signal_connect(
        item->button,
        "leave-notify-event",
        G_CALLBACK(dock_drawer_leave_notify),
        drawer);

    g_signal_connect(
        item->button,
        "enter-notify-event",
        G_CALLBACK(dock_enter_notify),
        dock);

    /*
     * Let the Dock hover policy run as well as the Drawer's own hover
     * open/close handling. This makes Auto Raise & Lower consistent across
     * every Dock slot.
     */
    g_signal_connect(
        item->button,
        "leave-notify-event",
        G_CALLBACK(dock_leave_notify),
        dock);

    g_signal_connect(
        item->button,
        "button-press-event",
        G_CALLBACK(dock_button_press),
        dock);

    g_signal_connect(
        item->button,
        "motion-notify-event",
        G_CALLBACK(dock_button_motion),
        dock);

    g_signal_connect_after(
        item->button,
        "button-release-event",
        G_CALLBACK(dock_button_release),
        dock);

    gtk_widget_show_all(item->button);

    g_ptr_array_add(
        dock->items,
        item);

    for (gsize i = 0;
         i < desktop_id_count;
         i++) {
        dock_drawer_add_desktop_id(
            drawer,
            desktop_ids[i]);
    }

    dock_resize_to_items(dock);

    return TRUE;
}

static void
dock_add_drawer_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    (void)menu_item;

    Dock *dock = user_data;
    guint number = 1;

    while (TRUE) {
        gchar *name =
            g_strdup_printf(
                "Drawer %u",
                number);

        gboolean used = FALSE;

        for (guint i = 0;
             i < dock->items->len;
             i++) {
            DockItem *item =
                g_ptr_array_index(
                    dock->items,
                    i);

            if (item->is_drawer &&
                g_strcmp0(
                    item->drawer->name,
                    name) == 0) {
                used = TRUE;
                break;
            }
        }

        if (!used) {
            if (dock_add_drawer(
                    dock,
                    name,
                    NULL,
                    0)) {
                dock_save(dock);
            }
            g_free(name);
            break;
        }

        g_free(name);
        number++;
    }
}

static void
dock_rename_drawer_menu_item(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    (void)menu_item;

    DockDrawer *drawer = user_data;

    if (!drawer || !drawer->dock || !drawer->button)
        return;

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(
            drawer->button);

    if (!toplevel || !GTK_IS_WINDOW(toplevel))
        return;

    GtkWidget *dialog =
        gtk_dialog_new_with_buttons(
            "Rename Drawer",
            GTK_WINDOW(toplevel),
            GTK_DIALOG_MODAL |
            GTK_DIALOG_DESTROY_WITH_PARENT,
            "_Cancel",
            GTK_RESPONSE_CANCEL,
            "_Rename",
            GTK_RESPONSE_OK,
            NULL);

    GtkWidget *content =
        gtk_dialog_get_content_area(
            GTK_DIALOG(dialog));

    GtkWidget *entry =
        gtk_entry_new();

    gtk_entry_set_text(
        GTK_ENTRY(entry),
        drawer->name ? drawer->name : "Drawer");

    gtk_entry_set_activates_default(
        GTK_ENTRY(entry),
        TRUE);

    gtk_box_pack_start(
        GTK_BOX(content),
        entry,
        TRUE,
        TRUE,
        12);

    gtk_dialog_set_default_response(
        GTK_DIALOG(dialog),
        GTK_RESPONSE_OK);

    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(entry);
    gtk_editable_select_region(
        GTK_EDITABLE(entry),
        0,
        -1);

    gint response =
        gtk_dialog_run(
            GTK_DIALOG(dialog));

    if (response == GTK_RESPONSE_OK) {
        const gchar *text =
            gtk_entry_get_text(
                GTK_ENTRY(entry));

        gchar *name =
            g_strdup(text ? text : "");

        g_strstrip(name);

        if (*name == '\0') {
            g_free(name);
            name = g_strdup("Drawer");
        }

        if (g_strcmp0(drawer->name, name) != 0) {
            g_free(drawer->name);
            drawer->name = name;

            gtk_widget_set_tooltip_text(
                drawer->button,
                drawer->name);

            dock_save(drawer->dock);
        } else {
            g_free(name);
        }
    }

    gtk_widget_destroy(dialog);
}

static gchar *
dock_launcher_get_icon_string(
    GAppInfo *app_info)
{
    if (!app_info)
        return g_strdup("");

    if (G_IS_DESKTOP_APP_INFO(app_info)) {
        gchar *icon =
            g_desktop_app_info_get_string(
                G_DESKTOP_APP_INFO(app_info),
                "Icon");

        if (icon && *icon)
            return icon;

        g_free(icon);
    }

    GIcon *gicon =
        g_app_info_get_icon(app_info);

    if (gicon && G_IS_THEMED_ICON(gicon)) {
        const gchar *const *names =
            g_themed_icon_get_names(
                G_THEMED_ICON(gicon));

        if (names && names[0])
            return g_strdup(names[0]);
    }

    if (gicon && G_IS_FILE_ICON(gicon)) {
        GFile *file =
            g_file_icon_get_file(
                G_FILE_ICON(gicon));

        if (file)
            return g_file_get_path(file);
    }

    return g_strdup("");
}

static void
dock_show_launcher_edit_error(
    GtkWidget *parent,
    const gchar *message)
{
    GtkWidget *toplevel =
        gtk_widget_get_toplevel(parent);

    GtkWidget *dialog =
        gtk_message_dialog_new(
            GTK_WINDOW(
                GTK_IS_WINDOW(toplevel) ?
                toplevel :
                NULL),
            GTK_DIALOG_MODAL |
            GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_CLOSE,
            "%s",
            message ? message : "Unable to edit launcher");

    gtk_dialog_run(
        GTK_DIALOG(dialog));

    gtk_widget_destroy(dialog);
}

static void
dock_edit_launcher(
    Dock *dock,
    DockItem *item,
    DockDrawerEntry *entry)
{
    if (!dock ||
        (!item && !entry))
        return;

    const gchar *desktop_id =
        item ?
        item->desktop_id :
        entry->desktop_id;

    DockLauncher *launcher =
        item ?
        item->launcher :
        entry->launcher;

    GtkWidget *source_widget =
        item ?
        item->button :
        entry->button;

    if (!desktop_id ||
        !launcher ||
        !source_widget)
        return;

    GAppInfo *app_info =
        dock_launcher_get_app_info(launcher);

    if (!G_IS_DESKTOP_APP_INFO(app_info))
        return;

    GDesktopAppInfo *desktop_info =
        G_DESKTOP_APP_INFO(app_info);

    const gchar *current_name =
        g_app_info_get_display_name(app_info);

    gchar *current_exec =
        g_desktop_app_info_get_string(
            desktop_info,
            "Exec");

    gchar *current_icon =
        dock_launcher_get_icon_string(app_info);

    gboolean current_terminal =
        g_desktop_app_info_get_boolean(
            desktop_info,
            "Terminal");

    gboolean current_dbus_activatable =
        g_desktop_app_info_get_boolean(
            desktop_info,
            "DBusActivatable");

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(source_widget);

    if (!toplevel || !GTK_IS_WINDOW(toplevel))
        toplevel = gtk_widget_get_toplevel(dock->box);

    GtkWidget *dialog =
        gtk_dialog_new_with_buttons(
            "Edit Launcher",
            GTK_WINDOW(
                GTK_IS_WINDOW(toplevel) ?
                toplevel :
                NULL),
            GTK_DIALOG_MODAL |
            GTK_DIALOG_DESTROY_WITH_PARENT,
            "_Reset",
            GTK_RESPONSE_REJECT,
            "_Cancel",
            GTK_RESPONSE_CANCEL,
            "_Save",
            GTK_RESPONSE_OK,
            NULL);

    GtkWidget *content =
        gtk_dialog_get_content_area(
            GTK_DIALOG(dialog));

    GtkWidget *grid =
        gtk_grid_new();

    gtk_grid_set_row_spacing(
        GTK_GRID(grid),
        8);

    gtk_grid_set_column_spacing(
        GTK_GRID(grid),
        10);

    gtk_container_set_border_width(
        GTK_CONTAINER(grid),
        12);

    GtkWidget *name_label =
        gtk_label_new("Name");

    GtkWidget *exec_label =
        gtk_label_new("Command");

    GtkWidget *icon_label =
        gtk_label_new("Icon");

    GtkWidget *name_entry =
        gtk_entry_new();

    GtkWidget *exec_entry =
        gtk_entry_new();

    GtkWidget *icon_entry =
        gtk_entry_new();

    GtkWidget *terminal_button =
        gtk_check_button_new_with_label(
            "Run in terminal");

    gtk_label_set_xalign(
        GTK_LABEL(name_label),
        0.0);

    gtk_label_set_xalign(
        GTK_LABEL(exec_label),
        0.0);

    gtk_label_set_xalign(
        GTK_LABEL(icon_label),
        0.0);

    gtk_entry_set_text(
        GTK_ENTRY(name_entry),
        current_name ? current_name : "");

    gtk_entry_set_text(
        GTK_ENTRY(exec_entry),
        current_exec ? current_exec : "");

    gtk_entry_set_text(
        GTK_ENTRY(icon_entry),
        current_icon);

    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(terminal_button),
        current_terminal);

    gtk_entry_set_placeholder_text(
        GTK_ENTRY(icon_entry),
        "Theme icon name or image path");

    gtk_widget_set_hexpand(
        name_entry,
        TRUE);

    gtk_widget_set_hexpand(
        exec_entry,
        TRUE);

    gtk_widget_set_hexpand(
        icon_entry,
        TRUE);

    gtk_grid_attach(
        GTK_GRID(grid),
        name_label,
        0,
        0,
        1,
        1);

    gtk_grid_attach(
        GTK_GRID(grid),
        name_entry,
        1,
        0,
        1,
        1);

    gtk_grid_attach(
        GTK_GRID(grid),
        exec_label,
        0,
        1,
        1,
        1);

    gtk_grid_attach(
        GTK_GRID(grid),
        exec_entry,
        1,
        1,
        1,
        1);

    gtk_grid_attach(
        GTK_GRID(grid),
        icon_label,
        0,
        2,
        1,
        1);

    gtk_grid_attach(
        GTK_GRID(grid),
        icon_entry,
        1,
        2,
        1,
        1);

    gtk_grid_attach(
        GTK_GRID(grid),
        terminal_button,
        1,
        3,
        1,
        1);

    gtk_box_pack_start(
        GTK_BOX(content),
        grid,
        TRUE,
        TRUE,
        0);

    gtk_dialog_set_default_response(
        GTK_DIALOG(dialog),
        GTK_RESPONSE_OK);


    /*
     * Keep the Preferences dialog compact but structured. The section
     * headers deliberately use normal GTK labels rather than custom CSS.
     */
    gtk_grid_insert_row(GTK_GRID(grid), 9);
    GtkWidget *appearance_section =
        gtk_label_new(NULL);
    gtk_label_set_markup(
        GTK_LABEL(appearance_section),
        "<b>Appearance</b>");
    gtk_widget_set_halign(
        appearance_section,
        GTK_ALIGN_START);
    gtk_grid_attach(
        GTK_GRID(grid),
        appearance_section,
        0, 9, 2, 1);

    gtk_grid_insert_row(GTK_GRID(grid), 3);
    GtkWidget *window_section =
        gtk_label_new(NULL);
    gtk_label_set_markup(
        GTK_LABEL(window_section),
        "<b>Window behavior</b>");
    gtk_widget_set_halign(
        window_section,
        GTK_ALIGN_START);
    gtk_grid_attach(
        GTK_GRID(grid),
        window_section,
        0, 3, 2, 1);

    gtk_grid_insert_row(GTK_GRID(grid), 0);
    GtkWidget *placement_section =
        gtk_label_new(NULL);
    gtk_label_set_markup(
        GTK_LABEL(placement_section),
        "<b>Placement</b>");
    gtk_widget_set_halign(
        placement_section,
        GTK_ALIGN_START);
    gtk_grid_attach(
        GTK_GRID(grid),
        placement_section,
        0, 0, 2, 1);

    /*
     * Explain the most important modern Dock interactions without adding
     * another preferences option.
     */
    gtk_widget_set_tooltip_text(
        appearance_section,
        "Visual settings for launcher tiles and the Dock window.");
    gtk_widget_set_tooltip_text(
        window_section,
        "Window grouping, minimized windows, and shortcut semantics.");

    gtk_widget_show_all(dialog);

    gtk_widget_grab_focus(name_entry);
    gtk_editable_select_region(
        GTK_EDITABLE(name_entry),
        0,
        -1);

    gint response =
        gtk_dialog_run(
            GTK_DIALOG(dialog));

    if (response == GTK_RESPONSE_REJECT) {
        GError *error = NULL;

        if (!dock_launcher_reset_override(
                desktop_id,
                &error)) {
            gchar *message =
                g_strdup_printf(
                    "Unable to restore the original launcher:\n%s",
                    error ? error->message : "unknown error");

            dock_show_launcher_edit_error(
                dialog,
                message);

            g_free(message);
            g_clear_error(&error);
        } else {
            DockLauncher *replacement =
                dock_launcher_new_from_desktop_id(
                    desktop_id);

            if (replacement) {
                if (item) {
                    DockLauncher *old =
                        item->launcher;
                    item->launcher = replacement;

                    dock_icon_button_set_launcher(
                        item->button,
                        replacement);

                    dock_launcher_free(old);
                } else {
                    DockLauncher *old =
                        entry->launcher;
                    entry->launcher = replacement;

                    dock_icon_button_set_launcher(
                        entry->button,
                        replacement);

                    dock_launcher_free(old);
                }
            }
        }
    } else if (response == GTK_RESPONSE_OK) {
        gchar *name =
            g_strdup(
                gtk_entry_get_text(
                    GTK_ENTRY(name_entry)));

        gchar *exec =
            g_strdup(
                gtk_entry_get_text(
                    GTK_ENTRY(exec_entry)));

        gchar *icon =
            g_strdup(
                gtk_entry_get_text(
                    GTK_ENTRY(icon_entry)));

        g_strstrip(name);
        g_strstrip(exec);
        g_strstrip(icon);

        if (*name == '\0' ||
            (*exec == '\0' &&
             !current_dbus_activatable)) {
            dock_show_launcher_edit_error(
                dialog,
                "Name and Command are required.");
        } else {
            GError *error = NULL;

            gboolean saved =
                dock_launcher_save_override(
                    desktop_id,
                    name,
                    exec,
                    icon,
                    gtk_toggle_button_get_active(
                        GTK_TOGGLE_BUTTON(terminal_button)),
                    current_dbus_activatable && *exec == '\0',
                    &error);

            if (!saved) {
                gchar *message =
                    g_strdup_printf(
                        "Unable to save the launcher override:\n%s",
                        error ? error->message : "unknown error");

                dock_show_launcher_edit_error(
                    dialog,
                    message);

                g_free(message);
                g_clear_error(&error);
            } else {
                DockLauncher *replacement =
                    dock_launcher_new_from_desktop_id(
                        desktop_id);

                if (!replacement) {
                    dock_show_launcher_edit_error(
                        dialog,
                        "The launcher override was saved, but it could not be loaded.");
                } else if (item) {
                    DockLauncher *old =
                        item->launcher;
                    item->launcher = replacement;

                    dock_icon_button_set_launcher(
                        item->button,
                        replacement);

                    dock_launcher_free(old);
                } else {
                    DockLauncher *old =
                        entry->launcher;
                    entry->launcher = replacement;

                    dock_icon_button_set_launcher(
                        entry->button,
                        replacement);

                    dock_launcher_free(old);
                }
            }
        }

        g_free(name);
        g_free(exec);
        g_free(icon);
    }

    gtk_widget_destroy(dialog);
    g_free(current_exec);
    g_free(current_icon);
}

static void
dock_edit_launcher_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    (void)menu_item;

    GtkWidget *source =
        g_object_get_data(
            G_OBJECT(menu_item),
            "dock-launcher-source");

    DockItem *item =
        source ?
        g_object_get_data(
            G_OBJECT(source),
            "dock-item") :
        NULL;

    dock_edit_launcher(
        user_data,
        item,
        NULL);
}

static void
dock_edit_drawer_launcher_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    DockDrawerEntry *entry =
        g_object_get_data(
            G_OBJECT(menu_item),
            "dock-drawer-entry");

    dock_edit_launcher(
        user_data,
        NULL,
        entry);
}

static void
dock_append_about_menu_item(
    GtkMenuShell *menu,
    Dock *dock)
{
    if (!GTK_IS_MENU_SHELL(menu) ||
        !dock)
        return;

    GtkWidget *about =
        dock_menu_item_new(
            "About",
            "help-about");

    g_signal_connect(
        about,
        "activate",
        G_CALLBACK(
            dock_about_menu_item_activated),
        dock);

    gtk_menu_shell_append(
        menu,
        about);
}

static void
dock_build_drawer_menu(
    GtkWidget *menu,
    Dock *dock)
{
    DockItem *item =
        g_object_get_data(
            G_OBJECT(menu),
            "dock-item");

    if (!item || !item->drawer)
        return;

    GtkWidget *open =
        dock_menu_item_new(
            "Open Drawer",
            "folder-open");

    g_signal_connect_swapped(
        open,
        "activate",
        G_CALLBACK(dock_drawer_toggle),
        item->drawer);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        open);

    GtkWidget *rename =
        dock_menu_item_new(
            "Rename Drawer",
            "document-edit");

    g_signal_connect(
        rename,
        "activate",
        G_CALLBACK(
            dock_rename_drawer_menu_item),
        item->drawer);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        rename);

    GtkWidget *add =
        dock_menu_item_new(
            "Add Application",
            "list-add");

    g_signal_connect(
        add,
        "activate",
        G_CALLBACK(
            dock_add_drawer_application_menu_item_activated),
        item->drawer);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        add);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    GtkWidget *remove =
        dock_menu_item_new(
            "Remove Drawer",
            "user-trash");

    g_object_set_data(
        G_OBJECT(remove),
        "dock-item",
        item);

    g_signal_connect(
        remove,
        "activate",
        G_CALLBACK(dock_remove_menu_item),
        dock);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        remove);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    dock_append_monitor_menu(
        GTK_MENU_SHELL(menu),
        dock);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    GtkWidget *swap_side =
        dock_menu_item_new(
            "Swap Dock Side",
            "object-flip-horizontal");

    g_signal_connect(
        swap_side,
        "activate",
        G_CALLBACK(
            dock_swap_side_menu_item_activated),
        dock);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        swap_side);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    GtkWidget *add_drawer =
        dock_menu_item_new(
            "Add Drawer",
            "folder-new");

    g_signal_connect(
        add_drawer,
        "activate",
        G_CALLBACK(dock_add_drawer_menu_item_activated),
        dock);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        add_drawer);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    dock_append_about_menu_item(
        GTK_MENU_SHELL(menu),
        dock);
}

static GdkMonitor *
dock_get_current_monitor(
    GtkWidget *toplevel)
{
    if (!toplevel)
        return NULL;

    GdkDisplay *display =
        gtk_widget_get_display(toplevel);

    if (!display)
        return NULL;

    GtkAllocation allocation;
    gtk_widget_get_allocation(
        toplevel,
        &allocation);

    gint window_x = 0;
    gint window_y = 0;

    gtk_window_get_position(
        GTK_WINDOW(toplevel),
        &window_x,
        &window_y);

    gint center_x =
        window_x +
        allocation.width / 2;

    gint center_y =
        window_y +
        allocation.height / 2;

    GdkMonitor *monitor =
        gdk_display_get_monitor_at_point(
            display,
            center_x,
            center_y);

    if (!monitor)
        monitor =
            gdk_display_get_primary_monitor(
                display);

    return monitor;
}


static void
dock_monitor_menu_item_toggled(
    GtkCheckMenuItem *menu_item,
    gpointer user_data)
{
    Dock *dock = user_data;

    if (!dock ||
        !gtk_check_menu_item_get_active(menu_item))
        return;

    gint monitor_index =
        GPOINTER_TO_INT(
            g_object_get_data(
                G_OBJECT(menu_item),
                "dock-monitor-index"));

    dock_set_monitor_index(
        dock,
        monitor_index);

    dock_save(dock);
}

static void
dock_append_monitor_menu(
    GtkMenuShell *menu,
    Dock *dock)
{
    if (!menu ||
        !dock)
        return;

    GtkWidget *move =
        dock_menu_item_new(
            "Move Dock to Monitor",
            "video-display");

    GtkWidget *submenu =
        gtk_menu_new();

    gtk_menu_set_reserve_toggle_size(
        GTK_MENU(submenu),
        FALSE);

    GdkDisplay *display =
        gtk_widget_get_display(
            dock->box);

    gint monitor_count =
        display ?
        gdk_display_get_n_monitors(display) :
        0;

    if (monitor_count <= 0) {
        GtkWidget *empty =
            gtk_menu_item_new_with_label(
                "No monitors available");

        gtk_widget_set_sensitive(
            empty,
            FALSE);

        gtk_menu_shell_append(
            GTK_MENU_SHELL(submenu),
            empty);
    } else {
        GtkWidget *radio = NULL;

        for (gint i = 0;
             i < monitor_count;
             i++) {
            GdkMonitor *monitor =
                gdk_display_get_monitor(
                    display,
                    i);

            if (!monitor)
                continue;

            GdkRectangle geometry;
            gdk_monitor_get_geometry(
                monitor,
                &geometry);

            gboolean primary =
                monitor ==
                gdk_display_get_primary_monitor(
                    display);

            const gchar *manufacturer =
                gdk_monitor_get_manufacturer(
                    monitor);

            const gchar *model =
                gdk_monitor_get_model(
                    monitor);

            gchar *identity = NULL;

            if (manufacturer &&
                *manufacturer &&
                model &&
                *model) {
                identity =
                    g_strdup_printf(
                        "%s %s",
                        manufacturer,
                        model);
            } else if (model &&
                       *model) {
                identity =
                    g_strdup(model);
            } else if (manufacturer &&
                       *manufacturer) {
                identity =
                    g_strdup(manufacturer);
            }

            gchar *label =
                identity ?
                g_strdup_printf(
                    "Monitor %d%s — %s — %dx%d",
                    i + 1,
                    primary ? " (Primary)" : "",
                    identity,
                    geometry.width,
                    geometry.height) :
                g_strdup_printf(
                    "Monitor %d%s — %dx%d",
                    i + 1,
                    primary ? " (Primary)" : "",
                    geometry.width,
                    geometry.height);

            g_free(identity);

            GtkWidget *item =
                radio ?
                gtk_radio_menu_item_new_with_label_from_widget(
                    GTK_RADIO_MENU_ITEM(radio),
                    label) :
                gtk_radio_menu_item_new_with_label(
                    NULL,
                    label);

            g_free(label);

            if (!radio)
                radio = item;

            g_object_set_data(
                G_OBJECT(item),
                "dock-monitor-index",
                GINT_TO_POINTER(i));

            gtk_check_menu_item_set_active(
                GTK_CHECK_MENU_ITEM(item),
                dock->monitor_index == i);

            g_signal_connect(
                item,
                "activate",
                G_CALLBACK(
                    dock_monitor_menu_item_toggled),
                dock);

            gtk_menu_shell_append(
                GTK_MENU_SHELL(submenu),
                item);
        }
    }

    gtk_menu_item_set_submenu(
        GTK_MENU_ITEM(move),
        submenu);

    gtk_menu_shell_append(
        menu,
        move);
}

static gboolean
dock_monitor_has_identity(const Dock *dock)
{
    return (dock->monitor_manufacturer &&
            *dock->monitor_manufacturer) ||
           (dock->monitor_model &&
            *dock->monitor_model);
}

static gboolean
dock_monitor_matches_identity(
    const Dock *dock,
    GdkMonitor *monitor)
{
    if (!dock ||
        !monitor ||
        !dock_monitor_has_identity(dock))
        return FALSE;

    gboolean have_manufacturer =
        dock->monitor_manufacturer &&
        *dock->monitor_manufacturer;
    gboolean have_model =
        dock->monitor_model &&
        *dock->monitor_model;

    return (!have_manufacturer ||
            g_strcmp0(
                dock->monitor_manufacturer,
                gdk_monitor_get_manufacturer(monitor)) == 0) &&
           (!have_model ||
            g_strcmp0(
                dock->monitor_model,
                gdk_monitor_get_model(monitor)) == 0);
}

static gint
dock_find_monitor_index_by_object(
    GdkDisplay *display,
    GdkMonitor *monitor)
{
    if (!display || !monitor)
        return -1;

    gint monitor_count =
        gdk_display_get_n_monitors(display);

    for (gint i = 0; i < monitor_count; i++) {
        if (gdk_display_get_monitor(display, i) == monitor)
            return i;
    }

    return -1;
}

static gint
dock_find_monitor_index_by_identity(
    Dock *dock,
    GdkDisplay *display)
{
    if (!dock ||
        !display ||
        !dock_monitor_has_identity(dock))
        return -1;

    gint monitor_count =
        gdk_display_get_n_monitors(display);

    for (gint i = 0; i < monitor_count; i++) {
        GdkMonitor *monitor =
            gdk_display_get_monitor(display, i);

        if (dock_monitor_matches_identity(dock, monitor))
            return i;
    }

    return -1;
}

static gint
dock_get_primary_monitor_index(GdkDisplay *display)
{
    if (!display)
        return -1;

    GdkMonitor *primary =
        gdk_display_get_primary_monitor(display);

    gint index =
        dock_find_monitor_index_by_object(
            display,
            primary);

    if (index >= 0)
        return index;

    return gdk_display_get_n_monitors(display) > 0 ? 0 : -1;
}

static void
dock_monitor_cache_selected(
    Dock *dock,
    GdkMonitor *monitor)
{
    if (!dock)
        return;

    g_set_object(
        &dock->monitor_object,
        monitor);

    g_free(dock->monitor_manufacturer);
    dock->monitor_manufacturer =
        monitor ?
        g_strdup(gdk_monitor_get_manufacturer(monitor)) :
        NULL;

    g_free(dock->monitor_model);
    dock->monitor_model =
        monitor ?
        g_strdup(gdk_monitor_get_model(monitor)) :
        NULL;
}

static gboolean
dock_monitor_refresh_idle(gpointer user_data)
{
    Dock *dock = user_data;

    if (!dock)
        return G_SOURCE_REMOVE;

    dock->monitor_refresh_id = 0;

    GdkDisplay *display = dock->monitor_display;
    if (!display)
        return G_SOURCE_REMOVE;

    gint monitor_count =
        gdk_display_get_n_monitors(display);

    if (monitor_count <= 0)
        return G_SOURCE_REMOVE;

    gint object_index =
        dock_find_monitor_index_by_object(
            display,
            dock->monitor_object);

    gint target_index = -1;
    gboolean identity_found = FALSE;

    /*
     * Keep the exact GdkMonitor object where possible; this distinguishes
     * two connected displays with the same manufacturer/model. If the
     * selected object disappeared, use its saved identity to find it again
     * (for example, after a display is reconnected).
     */
    if (object_index >= 0 &&
        dock_monitor_matches_identity(
            dock,
            gdk_display_get_monitor(display, object_index))) {
        target_index = object_index;
        identity_found = TRUE;
    } else {
        target_index =
            dock_find_monitor_index_by_identity(
                dock,
                display);
        identity_found = target_index >= 0;
    }

    if (target_index < 0 && object_index >= 0)
        target_index = object_index;

    if (target_index < 0)
        target_index =
            dock_get_primary_monitor_index(display);

    if (target_index < 0)
        return G_SOURCE_REMOVE;

    /*
     * dock_set_monitor_index() refreshes open Drawers as well as the Dock
     * position. If the selected display vanished, preserve its old identity
     * in memory so a later monitor-added event can restore the selection.
     */
    gboolean preserve_missing_identity =
        dock_monitor_has_identity(dock) &&
        !identity_found;

    gchar *saved_manufacturer =
        preserve_missing_identity ?
        g_strdup(dock->monitor_manufacturer) :
        NULL;
    gchar *saved_model =
        preserve_missing_identity ?
        g_strdup(dock->monitor_model) :
        NULL;

    dock_set_monitor_index(
        dock,
        target_index);

    if (preserve_missing_identity) {
        g_free(dock->monitor_manufacturer);
        dock->monitor_manufacturer = saved_manufacturer;

        g_free(dock->monitor_model);
        dock->monitor_model = saved_model;
    } else {
        /*
         * Keep the numeric index current after monitors are rearranged,
         * including setups where manufacturers/models are unavailable.
         */
        dock_save(dock);
    }

    return G_SOURCE_REMOVE;
}

static void
dock_schedule_monitor_refresh(Dock *dock)
{
    if (!dock ||
        dock->monitor_refresh_id)
        return;

    dock->monitor_refresh_id =
        g_idle_add(
            dock_monitor_refresh_idle,
            dock);
}

static void
dock_display_monitor_changed(
    GdkDisplay *display,
    GdkMonitor *monitor,
    gpointer user_data)
{
    (void)display;
    (void)monitor;

    dock_schedule_monitor_refresh(user_data);
}

static void
dock_screen_monitors_changed(
    GdkScreen *screen,
    gpointer user_data)
{
    (void)screen;

    dock_schedule_monitor_refresh(user_data);
}

static void
dock_position_window(Dock *dock)
{
    if (!dock || !dock->box)
        return;

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(dock->box);

    if (!toplevel || !GTK_IS_WINDOW(toplevel))
        return;

    GdkDisplay *display =
        gtk_widget_get_display(toplevel);

    if (!display)
        return;

    GdkMonitor *monitor = NULL;
    gint monitor_count =
        gdk_display_get_n_monitors(display);

    gboolean saved_monitor_valid =
        dock->monitor_index >= 0 &&
        dock->monitor_index < monitor_count;

    if (saved_monitor_valid) {
        monitor =
            gdk_display_get_monitor(
                display,
                dock->monitor_index);
    }

    if (!monitor)
        monitor =
            dock_get_current_monitor(toplevel);

    if (!monitor)
        monitor =
            gdk_display_get_primary_monitor(display);

    if (!monitor)
        return;

    /*
     * A configured monitor can disappear while the Dock is running.
     * Once we fall back to a real monitor, keep the stored index in sync.
     */
    if (!saved_monitor_valid) {
        for (gint i = 0; i < monitor_count; i++) {
            if (gdk_display_get_monitor(display, i) == monitor) {
                dock->monitor_index = i;
                dock_save(dock);
                break;
            }
        }
    }

    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);

    gint dock_width =
        gtk_widget_get_allocated_width(toplevel);
    if (dock_width <= 1)
        dock_width = dock_get_window_width(dock);

    gint dock_height =
        gtk_widget_get_allocated_height(toplevel);
    if (dock_height <= 1)
        dock_height = dock_get_window_height(dock);

    gint handle_height =
        dock_get_handle_height(dock);

    /*
     * Keep the Dock against the selected monitor's top and side edges.
     * The bottom button is the last child in the vertical column. Moving the
     * whole window up by the launcher area height leaves that button at the
     * top edge, just like a MATE panel's opposite-edge hide button.
     */
    gint x =
        dock->on_right_side ?
            geometry.x + geometry.width - dock_width :
            geometry.x;

    gint visible_y = geometry.y;

    gint y =
        dock->hidden ?
            geometry.y - dock_height + handle_height :
            visible_y;

    if (dock->hide_animating) {
        dock->hide_animation_target_x = x;
        dock->hide_animation_target_y = y;
    } else {
        gtk_window_move(
            GTK_WINDOW(toplevel),
            x,
            y);
    }

    if (!dock->hide_handle_button)
        return;

    if (!dock->show_hide_handle) {
        gtk_widget_hide(dock->hide_handle_button);
        return;
    }

    gint tile_size = dock_get_tile_size();

    gtk_widget_set_size_request(
        dock->hide_handle_button,
        tile_size,
        MAX(tile_size / 2, 1));

    gtk_fixed_move(
        GTK_FIXED(dock->box),
        dock->hide_handle_button,
        0,
        dock_get_handle_y(dock));

    if (dock->hide_handle_image) {
        gtk_image_set_from_icon_name(
            GTK_IMAGE(dock->hide_handle_image),
            dock->hidden ?
                "pan-down-symbolic" :
                "pan-up-symbolic",
            GTK_ICON_SIZE_BUTTON);
        gtk_image_set_pixel_size(
            GTK_IMAGE(dock->hide_handle_image),
            CLAMP((dock->icon_size * 3) / 8, 12, 18));
        gtk_widget_set_halign(
            dock->hide_handle_image,
            GTK_ALIGN_CENTER);
        gtk_widget_set_valign(
            dock->hide_handle_image,
            GTK_ALIGN_CENTER);
        gtk_widget_set_margin_start(
            dock->hide_handle_image, 0);
        gtk_widget_set_margin_end(
            dock->hide_handle_image, 0);
        gtk_widget_set_margin_top(
            dock->hide_handle_image, 0);
        gtk_widget_set_margin_bottom(
            dock->hide_handle_image, 0);
    }

    gtk_widget_set_tooltip_text(
        dock->hide_handle_button,
        dock->hidden ? "Show Dock" : "Hide Dock");

    if (!gtk_widget_get_visible(dock->hide_handle_button)) {
        gtk_widget_show(dock->hide_handle_button);
        if (dock->hide_handle_image)
            gtk_widget_show(dock->hide_handle_image);
    }
}

static gboolean
dock_hide_animation_tick(gpointer user_data)
{
    Dock *dock = user_data;

    if (!dock || !dock->box) {
        if (dock) {
            dock->hide_animation_tick_id = 0;
            dock->hide_animating = FALSE;
        }

        return G_SOURCE_REMOVE;
    }

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(
            dock->box);

    if (!toplevel ||
        !GTK_IS_WINDOW(toplevel)) {
        dock->hide_animation_tick_id = 0;
        dock->hide_animating = FALSE;
        return G_SOURCE_REMOVE;
    }

    gdouble progress =
        (gdouble)(g_get_monotonic_time() -
            dock->hide_animation_start_us) /
        DOCK_HIDE_ANIMATION_DURATION_US;

    progress = CLAMP(progress, 0.0, 1.0);

    /* Smoothstep easing gives the Dock a short, desktop-like slide. */
    gdouble eased =
        progress * progress * (3.0 - 2.0 * progress);

    gint x =
        (gint)round(
            dock->hide_animation_start_x +
            (dock->hide_animation_target_x -
                dock->hide_animation_start_x) * eased);

    gint y =
        (gint)round(
            dock->hide_animation_start_y +
            (dock->hide_animation_target_y -
                dock->hide_animation_start_y) * eased);

    gtk_window_move(
        GTK_WINDOW(toplevel),
        x,
        y);

    if (progress < 1.0)
        return G_SOURCE_CONTINUE;

    dock->hide_animation_tick_id = 0;
    dock->hide_animating = FALSE;

    gtk_window_move(
        GTK_WINDOW(toplevel),
        dock->hide_animation_target_x,
        dock->hide_animation_target_y);

    return G_SOURCE_REMOVE;
}

static void
dock_hide_handle_clicked(
    GtkButton *button,
    gpointer user_data)
{
    (void)button;

    Dock *dock = user_data;

    if (!dock ||
        !dock->box ||
        dock->hide_animating ||
        dock->dragging ||
        dock->dropping)
        return;

    if (!dock->hidden) {
        /* Avoid leaving popups floating after the main Dock slides away. */
        if (dock->x11)
            dock_x11_close_group_popup(dock->x11);

        for (guint i = 0;
             i < dock->items->len;
             i++) {
            DockItem *item =
                g_ptr_array_index(
                    dock->items,
                    i);

            if (item->is_drawer &&
                item->drawer &&
                item->drawer->popup) {
                dock_drawer_close(item->drawer);
            }
        }
    }

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(
            dock->box);

    if (!toplevel ||
        !GTK_IS_WINDOW(toplevel))
        return;

    gint start_x = 0;
    gint start_y = 0;

    gtk_window_get_position(
        GTK_WINDOW(toplevel),
        &start_x,
        &start_y);

    dock->hidden = !dock->hidden;
    dock->hide_animating = TRUE;
    dock->hide_animation_start_x = start_x;
    dock->hide_animation_start_y = start_y;
    dock->hide_animation_start_us = g_get_monotonic_time();
    dock->hide_animation_target_x = start_x;
    dock->hide_animation_target_y = start_y;

    /*
     * Recompute the destination and flip the arrow now, then let the
     * timer move the main Dock window rather than jumping instantly.
     */
    dock_position_window(dock);

    dock->hide_animation_tick_id =
        g_timeout_add(
            16,
            dock_hide_animation_tick,
            dock);
}

static void
dock_create_hide_handle(Dock *dock)
{
    if (!dock || !dock->box)
        return;

    GtkWidget *button =
        gtk_button_new();

    dock->hide_handle_button = button;
    gtk_widget_set_no_show_all(button, TRUE);

    gtk_button_set_relief(
        GTK_BUTTON(button),
        GTK_RELIEF_NORMAL);
    gtk_widget_set_can_focus(
        button,
        FALSE);
    gtk_widget_add_events(
        button,
        GDK_ENTER_NOTIFY_MASK |
        GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(
        button,
        "enter-notify-event",
        G_CALLBACK(dock_enter_notify),
        dock);
    g_signal_connect(
        button,
        "leave-notify-event",
        G_CALLBACK(dock_leave_notify),
        dock);

    /*
     * Keep the narrow reveal button predictable across GTK themes. The
     * requested size, rather than theme padding/minimums, defines how much
     * of the Dock remains visible when it is tucked behind the screen edge.
     */
    gtk_style_context_add_class(
        gtk_widget_get_style_context(button),
        "crepido-hide-handle");

    GtkCssProvider *css_provider =
        gtk_css_provider_new();

    gtk_css_provider_load_from_data(
        css_provider,
        "button.crepido-hide-handle { "
        "min-width: 0; min-height: 0; padding: 0; "
        "}",
        -1,
        NULL);

    gtk_style_context_add_provider(
        gtk_widget_get_style_context(button),
        GTK_STYLE_PROVIDER(css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_provider);

    gtk_widget_set_size_request(
        button,
        dock_get_tile_size(),
        MAX(dock_get_tile_size() / 2, 1));
    gtk_widget_set_tooltip_text(
        button,
        "Hide Dock");

    GtkWidget *image =
        gtk_image_new();

    dock->hide_handle_image = image;
    gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(image, 0);
    gtk_widget_set_margin_end(image, 0);
    gtk_widget_set_margin_top(image, 0);
    gtk_widget_set_margin_bottom(image, 0);

    gtk_container_add(
        GTK_CONTAINER(button),
        image);

    gtk_fixed_put(
        GTK_FIXED(dock->box),
        button,
        0,
        dock_get_handle_y(dock));

    g_signal_connect(
        button,
        "clicked",
        G_CALLBACK(dock_hide_handle_clicked),
        dock);

    if (dock->show_hide_handle) {
        gtk_widget_show(button);
        gtk_widget_show(image);
    } else {
        gtk_widget_hide(button);
    }
}

static void
dock_box_size_allocate(
    GtkWidget *widget,
    GtkAllocation *allocation,
    gpointer user_data)
{
    (void)widget;
    (void)allocation;

    Dock *dock = user_data;

    if (!dock ||
        dock->dragging ||
        dock->dropping)
        return;

    dock_position_window(dock);
}

static void
dock_preferences_mark_changed(
    GtkWidget *widget,
    gpointer user_data)
{
    (void)widget;

    GtkWidget *dialog = user_data;

    if (!dialog)
        return;

    gtk_widget_set_sensitive(
        gtk_dialog_get_widget_for_response(
            GTK_DIALOG(dialog),
            GTK_RESPONSE_OK),
        TRUE);
}

static void
dock_preferences_side_changed(
    GtkComboBox *combo,
    gpointer user_data)
{
    Dock *dock = user_data;

    if (!dock || !combo)
        return;

    const gchar *side_id =
        gtk_combo_box_get_active_id(combo);

    if (!side_id)
        return;

    dock_set_right_side(
        dock,
        g_strcmp0(side_id, "right") == 0);
}

static void
dock_preferences_position_changed(
    GtkComboBox *combo,
    gpointer user_data)
{
    Dock *dock = user_data;

    if (!dock || !combo)
        return;

    const gchar *id =
        gtk_combo_box_get_active_id(combo);

    DockPositionMode mode =
        g_strcmp0(id, "auto") == 0 ?
        DOCK_POSITION_AUTO_RAISE_LOWER :
        g_strcmp0(id, "top") == 0 ?
        DOCK_POSITION_KEEP_ON_TOP :
        DOCK_POSITION_NORMAL;

    dock_set_position_mode(
        dock,
        mode);
}

static void
dock_preferences_monitor_changed(
    GtkComboBox *combo,
    gpointer user_data)
{
    Dock *dock = user_data;

    if (!dock || !combo)
        return;

    const gchar *id =
        gtk_combo_box_get_active_id(combo);

    if (!id)
        return;

    dock_set_monitor_index(
        dock,
        (gint)g_ascii_strtoll(
            id,
            NULL,
            10));
}

static void
dock_preferences_shortcut_only_toggled(
    GtkToggleButton *button,
    gpointer user_data)
{
    Dock *dock = user_data;

    if (!dock || !button)
        return;

    dock_set_shortcut_only(
        dock,
        gtk_toggle_button_get_active(button));
}

static void
dock_preferences_minimized_window_icons_toggled(
    GtkToggleButton *button,
    gpointer user_data)
{
    Dock *dock = user_data;

    if (!dock || !button)
        return;

    dock_set_minimized_window_icons(
        dock,
        gtk_toggle_button_get_active(button));
}

static void
dock_preferences_minimized_title_labels_toggled(
    GtkToggleButton *button,
    gpointer user_data)
{
    Dock *dock = user_data;

    if (!dock || !button)
        return;

    dock_set_minimized_title_labels(
        dock,
        gtk_toggle_button_get_active(button));
}

static void
dock_preferences_minimized_group_drawer_toggled(
    GtkToggleButton *button,
    gpointer user_data)
{
    Dock *dock = user_data;

    if (!dock || !button)
        return;

    dock_set_minimized_group_drawer(
        dock,
        gtk_toggle_button_get_active(button));
}

static void
dock_preferences_minimized_all_workspaces_toggled(
    GtkToggleButton *button,
    gpointer user_data)
{
    Dock *dock = user_data;

    if (!dock || !button)
        return;

    dock_set_minimized_all_workspaces(
        dock,
        gtk_toggle_button_get_active(button));
}

static void
dock_preferences_indicator_toggled(
    GtkToggleButton *button,
    gpointer user_data)
{
    Dock *dock = user_data;

    if (!dock || !button)
        return;

    dock_set_show_window_indicator(
        dock,
        gtk_toggle_button_get_active(button));
}

static void
dock_preferences_show_hide_handle_toggled(
    GtkToggleButton *button,
    gpointer user_data)
{
    Dock *dock = user_data;

    if (!dock || !button)
        return;

    dock_set_show_hide_handle(
        dock,
        gtk_toggle_button_get_active(button));
}

static void
dock_preferences_icon_size_changed(
    GtkComboBox *combo,
    gpointer user_data)
{
    Dock *dock = user_data;

    if (!dock || !combo)
        return;

    const gchar *id =
        gtk_combo_box_get_active_id(combo);

    if (!id)
        return;

    dock_set_icon_size(
        dock,
        (gint)g_ascii_strtoll(
            id,
            NULL,
            10));
}

static void
dock_preferences_opacity_changed(
    GtkRange *range,
    gpointer user_data)
{
    Dock *dock = user_data;

    if (!dock || !range)
        return;

    dock_set_opacity(
        dock,
        (gint)round(
            gtk_range_get_value(range)));
}

static void
dock_preferences_bitmap_background_toggled(
    GtkToggleButton *button,
    gpointer user_data)
{
    (void)user_data;
    if (!button)
        return;
    gboolean enabled = gtk_toggle_button_get_active(button);
    GtkWidget *chooser =
        g_object_get_data(
            G_OBJECT(button),
            "crepido-bitmap-background-chooser");
    GtkWidget *mode_combo =
        g_object_get_data(
            G_OBJECT(button),
            "crepido-bitmap-background-mode");
    GtkWidget *image_label =
        g_object_get_data(
            G_OBJECT(button),
            "crepido-bitmap-background-label");
    GtkWidget *mode_label =
        g_object_get_data(
            G_OBJECT(button),
            "crepido-bitmap-background-mode-label");

    if (chooser)
        gtk_widget_set_sensitive(chooser, enabled);
    if (mode_combo)
        gtk_widget_set_sensitive(mode_combo, enabled);
    if (image_label)
        gtk_widget_set_sensitive(image_label, enabled);
    if (mode_label)
        gtk_widget_set_sensitive(mode_label, enabled);
}

static void
dock_preferences_style_section_frame(
    GtkWidget *frame,
    const gchar *title)
{
    if (!frame || !GTK_IS_FRAME(frame) || !title)
        return;

    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_widget_set_halign(label, GTK_ALIGN_START);

    gtk_frame_set_label_widget(GTK_FRAME(frame), label);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);
}

static GtkWidget *
dock_preferences_create_scrolled_page(
    GtkWidget *child)
{
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);

    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scrolled),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(
        GTK_SCROLLED_WINDOW(scrolled),
        GTK_SHADOW_NONE);
    gtk_container_add(GTK_CONTAINER(scrolled), child);

    return scrolled;
}

static void
dock_about_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    (void)menu_item;

    Dock *dock = user_data;

    if (!dock ||
        dock->dragging ||
        dock->dropping)
        return;

    GtkWidget *toplevel =
        dock->box ?
        gtk_widget_get_toplevel(dock->box) :
        NULL;

    GtkWindow *parent =
        toplevel && GTK_IS_WINDOW(toplevel) ?
        GTK_WINDOW(toplevel) :
        NULL;

    const gchar *authors[] = {
        "Crepido author: loonylynn",
        "MATE Panel upstream contributors:",
        "Perberos",
        "Steve Zesch",
        "Stefano Karapetsas",
        "GNOME Panel upstream contributors:",
        "George Lebl",
        "Jacob Berkman",
        "Miguel de Icaza",
        "Federico Mena",
        "Tom Tromey",
        "Ian Main",
        "Elliot Lee",
        "Owen Taylor",
        "Mark McLoughlin",
        "Alex Larsson",
        "Martin Baulig",
        "Seth Nickell",
        "Darin Adler",
        "Glynn Foster",
        "Stephen Browne",
        "Anders Carlsson",
        "Padraig O'Briain",
        "Ian McKellar",
        "Arvind Samptur",
        "Vincent Untz",
        NULL
    };

    const gchar *license_text =
        "Crepido is free software: you can redistribute it and/or modify it "
        "under the terms of the GNU General Public License as published by "
        "the Free Software Foundation, either version 2 of the License, or "
        "(at your option) any later version.\n\n"
        "Crepido is distributed in the hope that it will be useful, but "
        "WITHOUT ANY WARRANTY; without even the implied warranty of "
        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU "
        "General Public License for more details.";

    gtk_show_about_dialog(
        parent,
        "program-name",
        "Crepido",
        "version",
        "0.1.0",
        "copyright",
        "Copyright © 1997-2003 Free Software Foundation, Inc.\n"
        "Copyright © 2004 Vincent Untz\n"
        "Copyright © 2011-2021 MATE developers",
        "comments",
        "A GTK3/X11 desktop Dock with application launchers, Drawers, and "
        "minimized-window handling. Designed for MATE/Marco on X11 and "
        "intended to complement the desktop's normal panel.",
        "website",
        "https://github.com/loonylynn/crepido",
        "website-label",
        "Crepido project page",
        "authors",
        authors,
        "logo-icon-name",
        "user-desktop",
        "license-type",
        GTK_LICENSE_CUSTOM,
        "license",
        license_text,
        "wrap-license",
        TRUE,
        NULL);
}


static void
dock_quit_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    (void)menu_item;

    Dock *dock = user_data;

    if (!dock ||
        dock->dragging ||
        dock->dropping)
        return;

    GtkWidget *toplevel =
        dock->box ?
        gtk_widget_get_toplevel(dock->box) :
        NULL;

    if (!toplevel ||
        !GTK_IS_WINDOW(toplevel))
        return;

    GtkApplication *app =
        gtk_window_get_application(GTK_WINDOW(toplevel));

    /*
     * Let the application loop return before tearing down the Dock and its
     * child widgets. main() then releases the Dock model while its widgets
     * are still valid, avoiding a second destroy during window teardown.
     */
    if (app)
        g_application_quit(G_APPLICATION(app));
}
static void
dock_preferences_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    static gint last_preferences_page = 0;

    (void)menu_item;

    Dock *dock = user_data;

    if (!dock ||
        dock->dragging ||
        dock->dropping)
        return;

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(dock->box);

    if (!toplevel ||
        !GTK_IS_WINDOW(toplevel))
        return;

    GtkWidget *dialog =
        gtk_dialog_new_with_buttons(
            "Dock Preferences",
            GTK_WINDOW(toplevel),
            GTK_DIALOG_MODAL |
            GTK_DIALOG_DESTROY_WITH_PARENT,
            "_Apply",
            GTK_RESPONSE_OK,
            "_Cancel",
            GTK_RESPONSE_CANCEL,
            NULL);

    /* Keep the dialog usable on small displays while allowing its pages
     * to scroll when the available vertical space is limited. */
    gtk_window_set_resizable(
        GTK_WINDOW(dialog),
        TRUE);
    gtk_window_set_default_size(
        GTK_WINDOW(dialog),
        640,
        500);

    GtkWidget *content =
        gtk_dialog_get_content_area(
            GTK_DIALOG(dialog));

    gtk_container_set_border_width(
        GTK_CONTAINER(content),
        12);

    GtkWidget *notebook =
        gtk_notebook_new();

    gtk_notebook_set_tab_pos(
        GTK_NOTEBOOK(notebook),
        GTK_POS_TOP);

    gtk_box_pack_start(
        GTK_BOX(content),
        notebook,
        TRUE,
        TRUE,
        0);

    GtkWidget *placement_grid =
        gtk_grid_new();

    gtk_grid_set_row_spacing(
        GTK_GRID(placement_grid),
        18);

    gtk_grid_set_column_spacing(
        GTK_GRID(placement_grid),
        12);

    gtk_container_set_border_width(
        GTK_CONTAINER(placement_grid),
        12);

    GtkWidget *side_label =
        gtk_label_new_with_mnemonic("_Side:");
    gtk_widget_set_halign(
        side_label,
        GTK_ALIGN_START);

    GtkWidget *side_combo =
        gtk_combo_box_text_new();

    gtk_combo_box_text_append(
        GTK_COMBO_BOX_TEXT(side_combo),
        "left",
        "Left");
    gtk_combo_box_text_append(
        GTK_COMBO_BOX_TEXT(side_combo),
        "right",
        "Right");

    gtk_combo_box_set_active_id(
        GTK_COMBO_BOX(side_combo),
        dock->on_right_side ? "right" : "left");

    gtk_widget_set_hexpand(side_combo, TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(side_label), side_combo);
    gtk_widget_set_tooltip_text(
        side_combo,
        "Choose the screen edge used by the Dock.");

    gtk_grid_attach(
        GTK_GRID(placement_grid),
        side_label,
        0, 0, 1, 1);
    gtk_grid_attach(
        GTK_GRID(placement_grid),
        side_combo,
        1, 0, 1, 1);

    GtkWidget *position_label =
        gtk_label_new_with_mnemonic("_Position:");
    gtk_widget_set_halign(
        position_label,
        GTK_ALIGN_START);

    GtkWidget *position_combo =
        gtk_combo_box_text_new();

    gtk_combo_box_text_append(
        GTK_COMBO_BOX_TEXT(position_combo),
        "normal",
        "Normal");
    gtk_combo_box_text_append(
        GTK_COMBO_BOX_TEXT(position_combo),
        "auto",
        "Auto raise/lower");
    gtk_combo_box_text_append(
        GTK_COMBO_BOX_TEXT(position_combo),
        "top",
        "Top");

    const gchar *position_id =
        dock->position_mode ==
            DOCK_POSITION_AUTO_RAISE_LOWER ?
            "auto" :
        dock->position_mode ==
            DOCK_POSITION_KEEP_ON_TOP ?
            "top" :
            "normal";

    gtk_combo_box_set_active_id(
        GTK_COMBO_BOX(position_combo),
        position_id);

    gtk_widget_set_hexpand(position_combo, TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(position_label), position_combo);
    gtk_widget_set_tooltip_text(
        position_combo,
        "Normal stays behind windows. Auto raise/lower raises the Dock while the pointer is over it. Top keeps the Dock above normal windows.");

    gtk_grid_attach(
        GTK_GRID(placement_grid),
        position_label,
        0, 1, 1, 1);
    gtk_grid_attach(
        GTK_GRID(placement_grid),
        position_combo,
        1, 1, 1, 1);

    GtkWidget *position_frame =
        gtk_frame_new("Position modes");
    dock_preferences_style_section_frame(
        position_frame,
        "Position modes");

    GtkWidget *position_grid =
        gtk_grid_new();

    gtk_grid_set_row_spacing(
        GTK_GRID(position_grid),
        12);
    gtk_grid_set_column_spacing(
        GTK_GRID(position_grid),
        12);
    gtk_container_set_border_width(
        GTK_CONTAINER(position_grid),
        12);
    gtk_container_add(
        GTK_CONTAINER(position_frame),
        position_grid);

    GtkWidget *normal_label =
        gtk_label_new(NULL);
    gtk_label_set_markup(
        GTK_LABEL(normal_label),
        "<b>Normal</b>");
    gtk_widget_set_halign(
        normal_label,
        GTK_ALIGN_START);

    GtkWidget *normal_text =
        gtk_label_new(
            "The Dock stays behind other windows. "
            "Application windows can cover it.");
    gtk_label_set_xalign(GTK_LABEL(normal_text), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(normal_text), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(normal_text), 56);
    gtk_widget_set_halign(normal_text, GTK_ALIGN_START);
    gtk_widget_set_hexpand(normal_text, TRUE);

    gtk_grid_attach(GTK_GRID(position_grid), normal_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(position_grid), normal_text, 1, 0, 1, 1);

    GtkWidget *auto_label =
        gtk_label_new(NULL);
    gtk_label_set_markup(
        GTK_LABEL(auto_label),
        "<b>Auto raise/lower</b>");
    gtk_widget_set_halign(
        auto_label,
        GTK_ALIGN_START);

    GtkWidget *auto_text =
        gtk_label_new(
            "Raises the Dock when the pointer enters it, then lowers it "
            "behind other windows when the pointer leaves.");
    gtk_label_set_xalign(GTK_LABEL(auto_text), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(auto_text), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(auto_text), 56);
    gtk_widget_set_halign(auto_text, GTK_ALIGN_START);
    gtk_widget_set_hexpand(auto_text, TRUE);

    gtk_grid_attach(GTK_GRID(position_grid), auto_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(position_grid), auto_text, 1, 1, 1, 1);

    GtkWidget *top_label =
        gtk_label_new(NULL);
    gtk_label_set_markup(
        GTK_LABEL(top_label),
        "<b>Top</b>");
    gtk_widget_set_halign(
        top_label,
        GTK_ALIGN_START);

    GtkWidget *top_text =
        gtk_label_new(
            "Keeps the Dock above normal application windows so it remains "
            "visible while you work.");
    gtk_label_set_xalign(GTK_LABEL(top_text), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(top_text), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(top_text), 56);
    gtk_widget_set_halign(top_text, GTK_ALIGN_START);
    gtk_widget_set_hexpand(top_text, TRUE);

    gtk_grid_attach(GTK_GRID(position_grid), top_label, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(position_grid), top_text, 1, 2, 1, 1);

    gtk_grid_attach(
        GTK_GRID(placement_grid),
        position_frame,
        0, 2, 2, 1);

    GtkWidget *monitor_label =
        gtk_label_new_with_mnemonic("_Monitor:");
    gtk_widget_set_halign(
        monitor_label,
        GTK_ALIGN_START);

    GtkWidget *monitor_combo =
        gtk_combo_box_text_new();

    GdkDisplay *display =
        gtk_widget_get_display(dock->box);

    gint monitor_count =
        display ?
        gdk_display_get_n_monitors(display) :
        0;

    for (gint i = 0;
         i < monitor_count;
         i++) {
        GdkMonitor *monitor =
            gdk_display_get_monitor(
                display,
                i);

        gchar *label = NULL;

        if (monitor) {
            GdkRectangle geometry;
            gdk_monitor_get_geometry(
                monitor,
                &geometry);

            const gchar *manufacturer =
                gdk_monitor_get_manufacturer(
                    monitor);
            const gchar *model =
                gdk_monitor_get_model(
                    monitor);

            if (manufacturer &&
                *manufacturer &&
                model &&
                *model) {
                label =
                    g_strdup_printf(
                        "Monitor %d — %s %s — %dx%d",
                        i + 1,
                        manufacturer,
                        model,
                        geometry.width,
                        geometry.height);
            } else if (model && *model) {
                label =
                    g_strdup_printf(
                        "Monitor %d — %s — %dx%d",
                        i + 1,
                        model,
                        geometry.width,
                        geometry.height);
            } else {
                label =
                    g_strdup_printf(
                        "Monitor %d — %dx%d",
                        i + 1,
                        geometry.width,
                        geometry.height);
            }
        } else {
            label =
                g_strdup_printf(
                    "Monitor %d",
                    i + 1);
        }

        gchar *id =
            g_strdup_printf("%d", i);

        gtk_combo_box_text_append(
            GTK_COMBO_BOX_TEXT(monitor_combo),
            id,
            label);

        g_free(id);
        g_free(label);
    }

    gtk_widget_set_hexpand(monitor_combo, TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(monitor_label), monitor_combo);
    gtk_widget_set_tooltip_text(
        monitor_combo,
        "Choose which physical monitor owns the Dock. The monitor is remembered when possible.");

    if (monitor_count > 0) {
        gchar *active_id =
            g_strdup_printf(
                "%d",
                dock->monitor_index);

        gtk_combo_box_set_active_id(
            GTK_COMBO_BOX(monitor_combo),
            active_id);

        g_free(active_id);
    }

    gtk_grid_attach(
        GTK_GRID(placement_grid),
        monitor_label,
        0, 3, 1, 1);
    gtk_grid_attach(
        GTK_GRID(placement_grid),
        monitor_combo,
        1, 3, 1, 1);

    gtk_notebook_append_page(
        GTK_NOTEBOOK(notebook),
        dock_preferences_create_scrolled_page(placement_grid),
        gtk_label_new("Placement"));

    GtkWidget *behavior_grid =
        gtk_box_new(
            GTK_ORIENTATION_VERTICAL,
            18);

    gtk_container_set_border_width(
        GTK_CONTAINER(behavior_grid),
        12);

    GtkWidget *launcher_frame =
        gtk_frame_new("Launcher behavior");
    dock_preferences_style_section_frame(
        launcher_frame,
        "Launcher behavior");

    GtkWidget *launcher_box =
        gtk_box_new(
            GTK_ORIENTATION_VERTICAL,
            6);

    gtk_container_set_border_width(
        GTK_CONTAINER(launcher_box),
        12);

    gtk_container_add(
        GTK_CONTAINER(launcher_frame),
        launcher_box);

    GtkWidget *indicator =
        gtk_check_button_new_with_mnemonic(
            "Show _running-window indicator");

    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(indicator),
        dock->show_window_indicator);

    gtk_widget_set_tooltip_text(
        indicator,
        "Show a small indicator with the number of running windows for a launcher.");

    gtk_box_pack_start(
        GTK_BOX(launcher_box),
        indicator,
        FALSE,
        FALSE,
        0);

    GtkWidget *shortcut_only =
        gtk_check_button_new_with_mnemonic(
            "Use _shortcut-only mode (show minimized windows separately)");

    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(shortcut_only),
        dock->shortcut_only);

    gtk_widget_set_tooltip_text(
        shortcut_only,
        "Keeps Dock icons as pure launch shortcuts. Minimized windows appear separately instead of turning the Dock into a taskbar.");

    gtk_box_pack_start(
        GTK_BOX(launcher_box),
        shortcut_only,
        FALSE,
        FALSE,
        0);

    gtk_box_pack_start(
        GTK_BOX(behavior_grid),
        launcher_frame,
        FALSE,
        FALSE,
        0);

    GtkWidget *minimized_frame =
        gtk_frame_new("Minimized windows");
    dock_preferences_style_section_frame(
        minimized_frame,
        "Minimized windows");

    GtkWidget *minimized_box =
        gtk_box_new(
            GTK_ORIENTATION_VERTICAL,
            6);

    gtk_container_set_border_width(
        GTK_CONTAINER(minimized_box),
        12);

    gtk_container_add(
        GTK_CONTAINER(minimized_frame),
        minimized_box);

    GtkWidget *minimized_window_icons =
        gtk_check_button_new_with_mnemonic(
            "Show minimized windows as square _icons");

    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(minimized_window_icons),
        dock->minimized_window_icons);

    gtk_widget_set_tooltip_text(
        minimized_window_icons,
        "Show minimized windows as compact square icons along the Dock edge.");

    gtk_box_pack_start(
        GTK_BOX(minimized_box),
        minimized_window_icons,
        FALSE,
        FALSE,
        0);

    GtkWidget *minimized_title_labels =
        gtk_check_button_new_with_mnemonic(
            "Experimental: show window _titles on minimized icons");

    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(minimized_title_labels),
        dock->minimized_title_labels);

    gtk_widget_set_tooltip_text(
        minimized_title_labels,
        "Experimental: add a small rectangle title label beneath each minimized icon.");

    gtk_box_pack_start(
        GTK_BOX(minimized_box),
        minimized_title_labels,
        FALSE,
        FALSE,
        0);

    GtkWidget *minimized_group_drawer =
        gtk_check_button_new_with_mnemonic(
            "Expand minimized window groups as a _Drawer");

    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(minimized_group_drawer),
        dock->minimized_group_drawer);

    gtk_widget_set_tooltip_text(
        minimized_group_drawer,
        "Expand multiple minimized windows from one application upward as a Drawer. Wheel cycling also works over the group.");

    gtk_box_pack_start(
        GTK_BOX(minimized_box),
        minimized_group_drawer,
        FALSE,
        FALSE,
        0);

    GtkWidget *minimized_all_workspaces =
        gtk_check_button_new_with_mnemonic(
            "Show minimized windows from _all workspaces");

    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(minimized_all_workspaces),
        dock->minimized_all_workspaces);

    gtk_widget_set_tooltip_text(
        minimized_all_workspaces,
        "Include minimized windows from other workspaces in the minimized strip.");

    gtk_box_pack_start(
        GTK_BOX(minimized_box),
        minimized_all_workspaces,
        FALSE,
        FALSE,
        0);

    gtk_box_pack_start(
        GTK_BOX(behavior_grid),
        minimized_frame,
        FALSE,
        FALSE,
        0);

    GtkWidget *interaction_frame =
        gtk_frame_new("Mouse interactions");
    dock_preferences_style_section_frame(
        interaction_frame,
        "Mouse interactions");

    GtkWidget *interaction_grid =
        gtk_grid_new();

    gtk_grid_set_row_spacing(
        GTK_GRID(interaction_grid),
        6);

    gtk_grid_set_column_spacing(
        GTK_GRID(interaction_grid),
        12);

    gtk_container_set_border_width(
        GTK_CONTAINER(interaction_grid),
        12);

    gtk_container_add(
        GTK_CONTAINER(interaction_frame),
        interaction_grid);

    GtkWidget *left_label =
        gtk_label_new(NULL);

    gtk_label_set_markup(
        GTK_LABEL(left_label),
        "<b>Left-click</b>");

    gtk_widget_set_halign(
        left_label,
        GTK_ALIGN_START);

    GtkWidget *left_text =
        gtk_label_new(
            "Launch the application if it is not running; otherwise activate "
            "its window or minimize it.");

    gtk_label_set_xalign(
        GTK_LABEL(left_text),
        0.0);
    gtk_label_set_line_wrap(GTK_LABEL(left_text), TRUE);
    gtk_widget_set_halign(left_text, GTK_ALIGN_START);
    gtk_widget_set_hexpand(left_text, TRUE);

    gtk_grid_attach(
        GTK_GRID(interaction_grid),
        left_label,
        0, 0, 1, 1);

    gtk_grid_attach(
        GTK_GRID(interaction_grid),
        left_text,
        1, 0, 1, 1);

    GtkWidget *right_label =
        gtk_label_new(NULL);

    gtk_label_set_markup(
        GTK_LABEL(right_label),
        "<b>Right-click</b>");

    gtk_widget_set_halign(
        right_label,
        GTK_ALIGN_START);

    GtkWidget *right_text =
        gtk_label_new(
            "Open the Dock context menu.");

    gtk_label_set_xalign(
        GTK_LABEL(right_text),
        0.0);
    gtk_label_set_line_wrap(GTK_LABEL(right_text), TRUE);
    gtk_widget_set_halign(right_text, GTK_ALIGN_START);
    gtk_widget_set_hexpand(right_text, TRUE);

    gtk_grid_attach(
        GTK_GRID(interaction_grid),
        right_label,
        0, 1, 1, 1);

    gtk_grid_attach(
        GTK_GRID(interaction_grid),
        right_text,
        1, 1, 1, 1);

    GtkWidget *wheel_label =
        gtk_label_new(NULL);

    gtk_label_set_markup(
        GTK_LABEL(wheel_label),
        "<b>Mouse wheel</b>");

    gtk_widget_set_halign(
        wheel_label,
        GTK_ALIGN_START);

    GtkWidget *wheel_text =
        gtk_label_new(
            "Cycle through an application's minimized windows when more than "
            "one window is minimized.");

    gtk_label_set_xalign(
        GTK_LABEL(wheel_text),
        0.0);
    gtk_label_set_line_wrap(GTK_LABEL(wheel_text), TRUE);
    gtk_widget_set_halign(wheel_text, GTK_ALIGN_START);
    gtk_widget_set_hexpand(wheel_text, TRUE);

    gtk_grid_attach(
        GTK_GRID(interaction_grid),
        wheel_label,
        0, 2, 1, 1);

    gtk_grid_attach(
        GTK_GRID(interaction_grid),
        wheel_text,
        1, 2, 1, 1);

    gtk_box_pack_start(
        GTK_BOX(behavior_grid),
        interaction_frame,
        FALSE,
        FALSE,
        0);

    gtk_notebook_append_page(
        GTK_NOTEBOOK(notebook),
        dock_preferences_create_scrolled_page(behavior_grid),
        gtk_label_new("Window behavior"));

    GtkWidget *appearance_grid =
        gtk_box_new(
            GTK_ORIENTATION_VERTICAL,
            18);

    gtk_container_set_border_width(
        GTK_CONTAINER(appearance_grid),
        12);

    GtkWidget *icons_frame =
        gtk_frame_new("Dock icons");
    dock_preferences_style_section_frame(
        icons_frame,
        "Dock icons");

    GtkWidget *icons_grid =
        gtk_grid_new();

    gtk_grid_set_row_spacing(
        GTK_GRID(icons_grid),
        6);

    gtk_grid_set_column_spacing(
        GTK_GRID(icons_grid),
        12);

    gtk_container_set_border_width(
        GTK_CONTAINER(icons_grid),
        12);

    gtk_container_add(
        GTK_CONTAINER(icons_frame),
        icons_grid);

    GtkWidget *icon_size_label =
        gtk_label_new_with_mnemonic("Icon _size:");
    gtk_widget_set_halign(
        icon_size_label,
        GTK_ALIGN_START);

    GtkWidget *icon_size_combo =
        gtk_combo_box_text_new();

    gtk_combo_box_text_append(
        GTK_COMBO_BOX_TEXT(icon_size_combo),
        "32",
        "Small (32 px)");
    gtk_combo_box_text_append(
        GTK_COMBO_BOX_TEXT(icon_size_combo),
        "40",
        "Medium (40 px)");
    gtk_combo_box_text_append(
        GTK_COMBO_BOX_TEXT(icon_size_combo),
        "48",
        "Default (48 px)");
    gtk_combo_box_text_append(
        GTK_COMBO_BOX_TEXT(icon_size_combo),
        "56",
        "Large (56 px)");
    gtk_combo_box_text_append(
        GTK_COMBO_BOX_TEXT(icon_size_combo),
        "64",
        "Extra large (64 px)");

    gchar *icon_size_id =
        g_strdup_printf(
            "%d",
            dock->icon_size);

    gtk_combo_box_set_active_id(
        GTK_COMBO_BOX(icon_size_combo),
        icon_size_id);
    gtk_label_set_mnemonic_widget(
        GTK_LABEL(icon_size_label),
        icon_size_combo);
    gtk_widget_set_hexpand(icon_size_combo, TRUE);

    gtk_widget_set_tooltip_text(
        icon_size_combo,
        "Choose the launcher icon size. The Dock tile scales with the icon.");

    g_free(icon_size_id);

    gtk_grid_attach(
        GTK_GRID(icons_grid),
        icon_size_label,
        0, 0, 1, 1);
    gtk_grid_attach(
        GTK_GRID(icons_grid),
        icon_size_combo,
        1, 0, 1, 1);

    gtk_box_pack_start(
        GTK_BOX(appearance_grid),
        icons_frame,
        FALSE,
        FALSE,
        0);

    GtkWidget *bitmap_background_frame =
        gtk_frame_new("Block backgrounds");
    dock_preferences_style_section_frame(
        bitmap_background_frame,
        "Block backgrounds");
    GtkWidget *bitmap_background_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(bitmap_background_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(bitmap_background_grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(bitmap_background_grid), 12);
    gtk_container_add(GTK_CONTAINER(bitmap_background_frame), bitmap_background_grid);

    GtkWidget *bitmap_background_check =
        gtk_check_button_new_with_mnemonic(
            "_Use a bitmap image as each block's background");
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(bitmap_background_check),
        dock->bitmap_background_enabled);
    gtk_widget_set_tooltip_text(
        bitmap_background_check,
        "Show the selected image behind Dock launcher tiles, Drawer tiles, and minimized-window blocks.");

    GtkWidget *bitmap_background_label = gtk_label_new_with_mnemonic("Background _image:");
    gtk_widget_set_halign(bitmap_background_label, GTK_ALIGN_START);
    gtk_widget_set_sensitive(
        bitmap_background_label,
        dock->bitmap_background_enabled);

    GtkWidget *bitmap_background_chooser =
        gtk_file_chooser_button_new(
            "Choose block background image",
            GTK_FILE_CHOOSER_ACTION_OPEN);
    gtk_widget_set_hexpand(bitmap_background_chooser, TRUE);
    gtk_widget_set_sensitive(
        bitmap_background_chooser,
        dock->bitmap_background_enabled);
    gtk_widget_set_tooltip_text(
        bitmap_background_chooser,
        "Choose a raster image to use behind each block.");

    GtkFileFilter *bitmap_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(bitmap_filter, "Bitmap images");
    gtk_file_filter_add_mime_type(bitmap_filter, "image/png");
    gtk_file_filter_add_mime_type(bitmap_filter, "image/jpeg");
    gtk_file_filter_add_mime_type(bitmap_filter, "image/bmp");
    gtk_file_filter_add_mime_type(bitmap_filter, "image/gif");
    gtk_file_filter_add_mime_type(bitmap_filter, "image/tiff");
    gtk_file_filter_add_mime_type(bitmap_filter, "image/webp");
    gtk_file_filter_add_mime_type(bitmap_filter, "image/x-xpixmap");
    gtk_file_chooser_add_filter(
        GTK_FILE_CHOOSER(bitmap_background_chooser),
        bitmap_filter);

    if (dock->bitmap_background_path && *dock->bitmap_background_path) {
        gtk_file_chooser_set_filename(
            GTK_FILE_CHOOSER(bitmap_background_chooser),
            dock->bitmap_background_path);
    }

    GtkWidget *bitmap_background_mode_label = gtk_label_new_with_mnemonic("Image _layout:");
    gtk_widget_set_halign(bitmap_background_mode_label, GTK_ALIGN_START);
    gtk_widget_set_sensitive(
        bitmap_background_mode_label,
        dock->bitmap_background_enabled);

    GtkWidget *bitmap_background_mode_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(
        GTK_COMBO_BOX_TEXT(bitmap_background_mode_combo),
        "repeat",
        "Repeat (tile at original size)");
    gtk_combo_box_text_append(
        GTK_COMBO_BOX_TEXT(bitmap_background_mode_combo),
        "extend",
        "Extend (stretch to fill each block)");
    gtk_combo_box_set_active_id(
        GTK_COMBO_BOX(bitmap_background_mode_combo),
        dock->bitmap_background_mode == DOCK_BITMAP_BACKGROUND_EXTEND ?
            "extend" : "repeat");
    gtk_label_set_mnemonic_widget(
        GTK_LABEL(bitmap_background_label),
        bitmap_background_chooser);
    gtk_label_set_mnemonic_widget(
        GTK_LABEL(bitmap_background_mode_label),
        bitmap_background_mode_combo);
    gtk_widget_set_hexpand(bitmap_background_mode_combo, TRUE);
    gtk_widget_set_sensitive(
        bitmap_background_mode_combo,
        dock->bitmap_background_enabled);
    gtk_widget_set_tooltip_text(
        bitmap_background_mode_combo,
        "Repeat preserves the image's pixel size; Extend scales it to fill each block.");

    GtkWidget *bitmap_background_description =
        gtk_label_new(
            "Repeat tiles the image at its original size. Extend stretches it to fill each block.");
    gtk_label_set_xalign(GTK_LABEL(bitmap_background_description), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(bitmap_background_description), TRUE);
    gtk_widget_set_halign(bitmap_background_description, GTK_ALIGN_START);
    gtk_widget_set_hexpand(bitmap_background_description, TRUE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(bitmap_background_description),
        "dim-label");

    gtk_grid_attach(GTK_GRID(bitmap_background_grid), bitmap_background_check, 0, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(bitmap_background_grid), bitmap_background_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(bitmap_background_grid), bitmap_background_chooser, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(bitmap_background_grid), bitmap_background_mode_label, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(bitmap_background_grid), bitmap_background_mode_combo, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(bitmap_background_grid), bitmap_background_description, 0, 3, 2, 1);

    g_object_set_data(
        G_OBJECT(bitmap_background_check),
        "crepido-bitmap-background-chooser",
        bitmap_background_chooser);
    g_object_set_data(
        G_OBJECT(bitmap_background_check),
        "crepido-bitmap-background-mode",
        bitmap_background_mode_combo);
    g_object_set_data(
        G_OBJECT(bitmap_background_check),
        "crepido-bitmap-background-label",
        bitmap_background_label);
    g_object_set_data(
        G_OBJECT(bitmap_background_check),
        "crepido-bitmap-background-mode-label",
        bitmap_background_mode_label);

    gtk_box_pack_start(
        GTK_BOX(appearance_grid),
        bitmap_background_frame,
        FALSE,
        FALSE,
        0);

    GtkWidget *window_frame =
        gtk_frame_new("Dock window");
    dock_preferences_style_section_frame(
        window_frame,
        "Dock window");

    GtkWidget *window_grid =
        gtk_grid_new();

    gtk_grid_set_row_spacing(
        GTK_GRID(window_grid),
        6);

    gtk_grid_set_column_spacing(
        GTK_GRID(window_grid),
        12);

    gtk_container_set_border_width(
        GTK_CONTAINER(window_grid),
        12);

    gtk_container_add(
        GTK_CONTAINER(window_frame),
        window_grid);

    GtkWidget *opacity_label =
        gtk_label_new_with_mnemonic("_Opacity:");
    gtk_widget_set_halign(
        opacity_label,
        GTK_ALIGN_START);

    GtkWidget *opacity_scale =
        gtk_scale_new_with_range(
            GTK_ORIENTATION_HORIZONTAL,
            50.0,
            100.0,
            1.0);

    gtk_scale_set_draw_value(
        GTK_SCALE(opacity_scale),
        TRUE);

    gtk_scale_set_value_pos(
        GTK_SCALE(opacity_scale),
        GTK_POS_RIGHT);

    gtk_range_set_value(
        GTK_RANGE(opacity_scale),
        dock->opacity_percent);
    gtk_label_set_mnemonic_widget(
        GTK_LABEL(opacity_label),
        opacity_scale);

    gtk_widget_set_tooltip_text(
        opacity_scale,
        "Adjust Dock transparency from 50% to fully opaque.");

    gtk_widget_set_hexpand(
        opacity_scale,
        TRUE);

    gtk_grid_attach(
        GTK_GRID(window_grid),
        opacity_label,
        0, 0, 1, 1);
    gtk_grid_attach(
        GTK_GRID(window_grid),
        opacity_scale,
        1, 0, 1, 1);

    GtkWidget *show_hide_handle_check =
        gtk_check_button_new_with_mnemonic(
            "Show _hide/reveal button");

    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(show_hide_handle_check),
        dock->show_hide_handle);

    gtk_widget_set_tooltip_text(
        show_hide_handle_check,
        "Show the compact half-height arrow button. When the Dock is hidden, the button stays at the screen edge so you can restore it.");

    gtk_grid_attach(
        GTK_GRID(window_grid),
        show_hide_handle_check,
        0, 1, 2, 1);

    gtk_box_pack_start(
        GTK_BOX(appearance_grid),
        window_frame,
        FALSE,
        FALSE,
        0);

    gtk_notebook_append_page(
        GTK_NOTEBOOK(notebook),
        dock_preferences_create_scrolled_page(appearance_grid),
        gtk_label_new("Appearance"));

    gtk_notebook_set_current_page(
        GTK_NOTEBOOK(notebook),
        CLAMP(last_preferences_page, 0, 2));

    g_signal_connect(
        side_combo,
        "changed",
        G_CALLBACK(
            dock_preferences_side_changed),
        dock);

    g_signal_connect(
        position_combo,
        "changed",
        G_CALLBACK(
            dock_preferences_position_changed),
        dock);

    g_signal_connect(
        monitor_combo,
        "changed",
        G_CALLBACK(
            dock_preferences_monitor_changed),
        dock);

    g_signal_connect(
        indicator,
        "toggled",
        G_CALLBACK(
            dock_preferences_indicator_toggled),
        dock);

    g_signal_connect(
        show_hide_handle_check,
        "toggled",
        G_CALLBACK(
            dock_preferences_show_hide_handle_toggled),
        dock);

    g_signal_connect(
        shortcut_only,
        "toggled",
        G_CALLBACK(
            dock_preferences_shortcut_only_toggled),
        dock);

    g_signal_connect(
        minimized_window_icons,
        "toggled",
        G_CALLBACK(
            dock_preferences_minimized_window_icons_toggled),
        dock);

    g_signal_connect(
        minimized_title_labels,
        "toggled",
        G_CALLBACK(
            dock_preferences_minimized_title_labels_toggled),
        dock);

    g_signal_connect(
        minimized_title_labels,
        "toggled",
        G_CALLBACK(
            dock_preferences_mark_changed),
        dialog);

    g_signal_connect(
        minimized_group_drawer,
        "toggled",
        G_CALLBACK(
            dock_preferences_minimized_group_drawer_toggled),
        dock);

    g_signal_connect(
        minimized_all_workspaces,
        "toggled",
        G_CALLBACK(
            dock_preferences_minimized_all_workspaces_toggled),
        dock);

    g_signal_connect(
        icon_size_combo,
        "changed",
        G_CALLBACK(
            dock_preferences_icon_size_changed),
        dock);

    g_signal_connect(
        opacity_scale,
        "value-changed",
        G_CALLBACK(
            dock_preferences_opacity_changed),
        dock);

    GtkWidget *apply_button =
        gtk_dialog_get_widget_for_response(
            GTK_DIALOG(dialog),
            GTK_RESPONSE_OK);

    gtk_widget_set_sensitive(
        apply_button,
        FALSE);
    gtk_dialog_set_default_response(
        GTK_DIALOG(dialog),
        GTK_RESPONSE_OK);

    g_signal_connect(
        side_combo,
        "changed",
        G_CALLBACK(
            dock_preferences_mark_changed),
        dialog);

    g_signal_connect(
        position_combo,
        "changed",
        G_CALLBACK(
            dock_preferences_mark_changed),
        dialog);

    g_signal_connect(
        monitor_combo,
        "changed",
        G_CALLBACK(
            dock_preferences_mark_changed),
        dialog);

    g_signal_connect(
        indicator,
        "toggled",
        G_CALLBACK(
            dock_preferences_mark_changed),
        dialog);

    g_signal_connect(
        show_hide_handle_check,
        "toggled",
        G_CALLBACK(
            dock_preferences_mark_changed),
        dialog);

    g_signal_connect(
        shortcut_only,
        "toggled",
        G_CALLBACK(
            dock_preferences_mark_changed),
        dialog);

    g_signal_connect(
        minimized_window_icons,
        "toggled",
        G_CALLBACK(
            dock_preferences_mark_changed),
        dialog);

    g_signal_connect(
        minimized_group_drawer,
        "toggled",
        G_CALLBACK(
            dock_preferences_mark_changed),
        dialog);

    g_signal_connect(
        minimized_all_workspaces,
        "toggled",
        G_CALLBACK(
            dock_preferences_mark_changed),
        dialog);

    g_signal_connect(
        icon_size_combo,
        "changed",
        G_CALLBACK(
            dock_preferences_mark_changed),
        dialog);

    g_signal_connect(
        opacity_scale,
        "value-changed",
        G_CALLBACK(
            dock_preferences_mark_changed),
        dialog);

    g_signal_connect(
        bitmap_background_check,
        "toggled",
        G_CALLBACK(dock_preferences_bitmap_background_toggled),
        NULL);
    g_signal_connect(
        bitmap_background_check,
        "toggled",
        G_CALLBACK(dock_preferences_mark_changed),
        dialog);
    g_signal_connect(
        bitmap_background_chooser,
        "file-set",
        G_CALLBACK(dock_preferences_mark_changed),
        dialog);
    g_signal_connect(
        bitmap_background_mode_combo,
        "changed",
        G_CALLBACK(dock_preferences_mark_changed),
        dialog);

    gtk_widget_show_all(dialog);

    /* Start keyboard focus on the first control in the selected tab. */
    switch (gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook))) {
    case 1:
        gtk_widget_grab_focus(indicator);
        break;
    case 2:
        gtk_widget_grab_focus(icon_size_combo);
        break;
    case 0:
    default:
        gtk_widget_grab_focus(side_combo);
        break;
    }

    gboolean committed_right =
        dock->on_right_side;
    DockPositionMode committed_position =
        dock->position_mode;
    gint committed_monitor =
        dock->monitor_index;
    gboolean committed_indicator =
        dock->show_window_indicator;
    gboolean committed_show_hide_handle =
        dock->show_hide_handle;
    gboolean committed_shortcut_only =
        dock->shortcut_only;
    gboolean committed_minimized_window_icons =
        dock->minimized_window_icons;
    gboolean committed_minimized_title_labels =
        dock->minimized_title_labels;
    gboolean committed_minimized_group_drawer =
        dock->minimized_group_drawer;
    gboolean committed_minimized_all_workspaces =
        dock->minimized_all_workspaces;
    gint committed_icon_size =
        dock->icon_size;
    gint committed_opacity =
        dock->opacity_percent;
    gboolean committed_bitmap_background_enabled =
        dock->bitmap_background_enabled;
    DockBitmapBackgroundMode committed_bitmap_background_mode =
        dock->bitmap_background_mode;
    gchar *committed_bitmap_background_path =
        g_strdup(dock->bitmap_background_path);

    gint response;

    do {
        response =
            gtk_dialog_run(
                GTK_DIALOG(dialog));

        if (response != GTK_RESPONSE_OK)
            break;

        const gchar *side_id =
            gtk_combo_box_get_active_id(
                GTK_COMBO_BOX(side_combo));
        const gchar *position_id_new =
            gtk_combo_box_get_active_id(
                GTK_COMBO_BOX(position_combo));
        const gchar *monitor_id =
            gtk_combo_box_get_active_id(
                GTK_COMBO_BOX(monitor_combo));

        DockPositionMode position =
            g_strcmp0(position_id_new, "auto") == 0 ?
            DOCK_POSITION_AUTO_RAISE_LOWER :
            g_strcmp0(position_id_new, "top") == 0 ?
            DOCK_POSITION_KEEP_ON_TOP :
            DOCK_POSITION_NORMAL;

        gint monitor_index =
            monitor_id ?
            (gint)g_ascii_strtoll(
                monitor_id,
                NULL,
                10) :
            dock->monitor_index;

        const gchar *icon_size_id_new =
            gtk_combo_box_get_active_id(
                GTK_COMBO_BOX(icon_size_combo));

        gint icon_size =
            icon_size_id_new ?
            (gint)g_ascii_strtoll(
                icon_size_id_new,
                NULL,
                10) :
            dock->icon_size;

        gint opacity_percent =
            (gint)round(
                gtk_range_get_value(
                    GTK_RANGE(opacity_scale)));

        gboolean bitmap_background_enabled =
            gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(bitmap_background_check));
        gchar *bitmap_background_path =
            gtk_file_chooser_get_filename(
                GTK_FILE_CHOOSER(bitmap_background_chooser));
        const gchar *bitmap_background_mode_id =
            gtk_combo_box_get_active_id(
                GTK_COMBO_BOX(bitmap_background_mode_combo));
        DockBitmapBackgroundMode bitmap_background_mode =
            g_strcmp0(bitmap_background_mode_id, "extend") == 0 ?
                DOCK_BITMAP_BACKGROUND_EXTEND :
                DOCK_BITMAP_BACKGROUND_REPEAT;

        if (bitmap_background_enabled &&
            (!bitmap_background_path ||
             !*bitmap_background_path)) {
            GtkWidget *warning =
                gtk_message_dialog_new(
                    GTK_WINDOW(dialog),
                    GTK_DIALOG_MODAL |
                        GTK_DIALOG_DESTROY_WITH_PARENT,
                    GTK_MESSAGE_WARNING,
                    GTK_BUTTONS_OK,
                    "Choose a background image or turn off block backgrounds.");
            gtk_dialog_run(GTK_DIALOG(warning));
            gtk_widget_destroy(warning);
            g_free(bitmap_background_path);
            continue;
        }

        dock_set_right_side(
            dock,
            g_strcmp0(side_id, "right") == 0);
        dock_set_position_mode(
            dock,
            position);
        dock_set_monitor_index(
            dock,
            monitor_index);
        dock_set_icon_size(
            dock,
            icon_size);
        dock_set_opacity(
            dock,
            opacity_percent);
        dock_set_show_window_indicator(
            dock,
            gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(indicator)));
        dock_set_show_hide_handle(
            dock,
            gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(show_hide_handle_check)));
        dock_set_shortcut_only(
            dock,
            gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(shortcut_only)));
        dock_set_minimized_window_icons(
            dock,
            gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(minimized_window_icons)));
        dock_set_minimized_title_labels(
            dock,
            gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(minimized_title_labels)));
        dock_set_minimized_group_drawer(
            dock,
            gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(minimized_group_drawer)));
        dock_set_minimized_all_workspaces(
            dock,
            gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(minimized_all_workspaces)));
        dock_set_bitmap_background(
            dock,
            bitmap_background_enabled,
            bitmap_background_path,
            bitmap_background_mode);
        g_free(bitmap_background_path);

        dock_save(dock);

        committed_right =
            dock->on_right_side;
        committed_position =
            dock->position_mode;
        committed_monitor =
            dock->monitor_index;
        committed_icon_size =
            dock->icon_size;
        committed_opacity =
            dock->opacity_percent;
        committed_indicator =
            dock->show_window_indicator;
        committed_show_hide_handle =
            dock->show_hide_handle;
        committed_shortcut_only =
            dock->shortcut_only;
        committed_minimized_window_icons =
            dock->minimized_window_icons;
        committed_minimized_title_labels =
            dock->minimized_title_labels;
        committed_minimized_group_drawer =
            dock->minimized_group_drawer;
        committed_minimized_all_workspaces =
            dock->minimized_all_workspaces;
        committed_bitmap_background_enabled =
            dock->bitmap_background_enabled;
        committed_bitmap_background_mode =
            dock->bitmap_background_mode;
        g_free(committed_bitmap_background_path);
        committed_bitmap_background_path =
            g_strdup(dock->bitmap_background_path);

        gtk_widget_set_sensitive(
            apply_button,
            FALSE);

        /*
         * Apply the new settings but keep the Preferences dialog open.
         * The new values are now the state that Cancel should restore to.
         */
    } while (response == GTK_RESPONSE_OK);

    if (response != GTK_RESPONSE_OK) {
        dock_set_right_side(
            dock,
            committed_right);
        dock_set_position_mode(
            dock,
            committed_position);
        dock_set_monitor_index(
            dock,
            committed_monitor);
        dock_set_icon_size(
            dock,
            committed_icon_size);
        dock_set_opacity(
            dock,
            committed_opacity);
        dock_set_show_window_indicator(
            dock,
            committed_indicator);
        dock_set_show_hide_handle(
            dock,
            committed_show_hide_handle);
        dock_set_shortcut_only(
            dock,
            committed_shortcut_only);
        dock_set_minimized_window_icons(
            dock,
            committed_minimized_window_icons);
        dock_set_minimized_title_labels(
            dock,
            committed_minimized_title_labels);
        dock_set_minimized_group_drawer(
            dock,
            committed_minimized_group_drawer);
        dock_set_minimized_all_workspaces(
            dock,
            committed_minimized_all_workspaces);
        dock_set_bitmap_background(
            dock,
            committed_bitmap_background_enabled,
            committed_bitmap_background_path,
            committed_bitmap_background_mode);
    }

    g_free(committed_bitmap_background_path);

    last_preferences_page =
        gtk_notebook_get_current_page(
            GTK_NOTEBOOK(notebook));

    gtk_widget_destroy(dialog);
}

static void
dock_swap_side_menu_item_activated(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    (void)menu_item;

    Dock *dock = user_data;

    if (!dock)
        return;

    dock_set_right_side(
        dock,
        !dock->on_right_side);

    dock_save(dock);
}

static void
dock_lower_window(
    Dock *dock)
{
    if (!dock || !dock->box)
        return;

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(
            dock->box);

    if (!toplevel ||
        !GTK_IS_WINDOW(toplevel))
        return;

    GdkWindow *window =
        gtk_widget_get_window(toplevel);

    if (window)
        gdk_window_lower(window);

}

static void
dock_raise_window(
    Dock *dock)
{
    if (!dock || !dock->box)
        return;

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(
            dock->box);

    if (!toplevel ||
        !GTK_IS_WINDOW(toplevel))
        return;

    GdkWindow *window =
        gtk_widget_get_window(toplevel);

    if (window)
        gdk_window_raise(window);

}

static gboolean
dock_enter_notify(
    GtkWidget *widget,
    GdkEventCrossing *event,
    gpointer user_data)
{
    (void)widget;

    Dock *dock = user_data;

    if (!dock ||
        event->detail == GDK_NOTIFY_INFERIOR)
        return FALSE;

    if (dock->position_mode ==
        DOCK_POSITION_AUTO_RAISE_LOWER) {
        dock_raise_window(dock);
    }

    return FALSE;
}

static gboolean
dock_leave_notify(
    GtkWidget *widget,
    GdkEventCrossing *event,
    gpointer user_data)
{
    (void)widget;

    Dock *dock = user_data;

    if (!dock ||
        event->detail == GDK_NOTIFY_INFERIOR)
        return FALSE;

    if (dock->position_mode ==
        DOCK_POSITION_AUTO_RAISE_LOWER) {
        dock_lower_window(dock);
    }

    return FALSE;
}

static void
dock_popup_menu_on_monitor(
    GtkMenu *menu,
    GtkWidget *widget,
    GdkEventButton *event,
    Dock *dock)
{
    if (!menu ||
        !widget ||
        !event ||
        !dock)
        return;

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(widget);

    GdkDisplay *display =
        (toplevel && GTK_IS_WIDGET(toplevel)) ?
        gtk_widget_get_display(toplevel) :
        NULL;

    if (display) {
        gint monitor_count =
            gdk_display_get_n_monitors(display);

        if (dock->monitor_index >= 0 &&
            dock->monitor_index < monitor_count) {
            gtk_menu_set_monitor(
                menu,
                dock->monitor_index);
        }
    }

    gtk_menu_popup_at_pointer(
        menu,
        (GdkEvent *)event);
}

static GtkTargetEntry dock_desktop_drop_targets[] = {
    {
        "text/uri-list",
        0,
        0
    }
};

static gchar *
dock_desktop_id_from_drop_uri(
    const gchar *uri)
{
    if (!uri ||
        !*uri)
        return NULL;

    GFile *file =
        g_file_new_for_uri(uri);

    if (!g_file_is_native(file)) {
        g_object_unref(file);
        return NULL;
    }

    gchar *path =
        g_file_get_path(file);

    g_object_unref(file);

    if (!path ||
        !g_file_test(
            path,
            G_FILE_TEST_IS_REGULAR) ||
        !g_str_has_suffix(
            path,
            ".desktop")) {
        g_free(path);
        return NULL;
    }

    return path;
}

static gboolean
dock_add_dropped_desktop_files(
    Dock *dock,
    DockDrawer *drawer,
    GtkSelectionData *selection_data)
{
    if (!dock ||
        !selection_data ||
        (drawer && drawer->dock != dock))
        return FALSE;

    gchar **uris =
        gtk_selection_data_get_uris(
            selection_data);

    if (!uris)
        return FALSE;

    gboolean added = FALSE;

    for (guint i = 0;
         uris[i];
         i++) {
        gchar *desktop_id =
            dock_desktop_id_from_drop_uri(
                uris[i]);

        if (!desktop_id)
            continue;

        gboolean item_added =
            drawer ?
            dock_drawer_add_desktop_id(
                drawer,
                desktop_id) :
            dock_add_desktop_id(
                dock,
                desktop_id);

        if (item_added)
            added = TRUE;

        g_free(desktop_id);
    }

    g_strfreev(uris);

    if (added)
        dock_save(dock);

    return added;
}

static void
dock_desktop_drag_data_received(
    GtkWidget *widget,
    GdkDragContext *context,
    gint x,
    gint y,
    GtkSelectionData *selection_data,
    guint info,
    guint time,
    gpointer user_data)
{
    (void)widget;
    (void)x;
    (void)y;
    (void)info;

    Dock *dock = user_data;

    gboolean added =
        dock_add_dropped_desktop_files(
            dock,
            NULL,
            selection_data);

    if (added) {
        GtkWidget *source =
            gtk_drag_get_source_widget(context);

        if (source &&
            GPOINTER_TO_INT(
                g_object_get_data(
                    G_OBJECT(source),
                    "dock-app-chooser-source"))) {
            GtkWidget *toplevel =
                gtk_widget_get_toplevel(source);

            if (toplevel &&
                GTK_IS_WINDOW(toplevel))
                gtk_widget_destroy(toplevel);
        }
    }

    gtk_drag_finish(
        context,
        added,
        FALSE,
        time);
}

static void
dock_enable_desktop_drop(
    GtkWidget *widget,
    Dock *dock)
{
    if (!widget ||
        !dock)
        return;

    gtk_drag_dest_set(
        widget,
        GTK_DEST_DEFAULT_ALL,
        dock_desktop_drop_targets,
        G_N_ELEMENTS(
            dock_desktop_drop_targets),
        GDK_ACTION_COPY);

    g_signal_connect(
        widget,
        "drag-data-received",
        G_CALLBACK(
            dock_desktop_drag_data_received),
        dock);
}

static void
dock_drawer_desktop_drag_data_received(
    GtkWidget *widget,
    GdkDragContext *context,
    gint x,
    gint y,
    GtkSelectionData *selection_data,
    guint info,
    guint time,
    gpointer user_data)
{
    (void)widget;
    (void)x;
    (void)y;
    (void)info;

    DockDrawer *drawer = user_data;

    gboolean added =
        drawer &&
        dock_add_dropped_desktop_files(
            drawer->dock,
            drawer,
            selection_data);

    if (added) {
        GtkWidget *source =
            gtk_drag_get_source_widget(context);

        if (source &&
            GPOINTER_TO_INT(
                g_object_get_data(
                    G_OBJECT(source),
                    "dock-app-chooser-source"))) {
            GtkWidget *toplevel =
                gtk_widget_get_toplevel(source);

            if (toplevel &&
                GTK_IS_WINDOW(toplevel))
                gtk_widget_destroy(toplevel);
        }
    }

    gtk_drag_finish(
        context,
        added,
        FALSE,
        time);
}

static void
dock_enable_desktop_drop_on_drawer(
    GtkWidget *widget,
    DockDrawer *drawer)
{
    if (!widget ||
        !drawer ||
        !drawer->dock)
        return;

    gtk_drag_dest_set(
        widget,
        GTK_DEST_DEFAULT_ALL,
        dock_desktop_drop_targets,
        G_N_ELEMENTS(
            dock_desktop_drop_targets),
        GDK_ACTION_COPY);

    g_signal_connect(
        widget,
        "drag-data-received",
        G_CALLBACK(
            dock_drawer_desktop_drag_data_received),
        drawer);
}

static gboolean
dock_popup_menu(
    GtkWidget *widget,
    GdkEventButton *event,
    gpointer user_data)
{
    if (event->button != GDK_BUTTON_SECONDARY)
        return FALSE;

    Dock *dock = user_data;
    DockItem *item =
        g_object_get_data(
            G_OBJECT(widget),
            "dock-item");

    GtkWidget *menu =
        gtk_menu_new();

    gtk_menu_set_reserve_toggle_size(
        GTK_MENU(menu),
        FALSE);

    if (item && item->is_drawer) {
        g_object_set_data(
            G_OBJECT(menu),
            "dock-item",
            item);

        dock_build_drawer_menu(
            menu,
            dock);

        gtk_widget_show_all(menu);

        dock_popup_menu_on_monitor(
            GTK_MENU(menu),
            widget,
            event,
            dock);

        return TRUE;
    }

    if (item) {
        GtkWidget *open =
            dock_menu_item_new(
            "Launch New Instance",
            "system-run");

        g_signal_connect(
            open,
            "activate",
            G_CALLBACK(dock_launch_menu_item),
            item);

        gtk_menu_shell_append(
            GTK_MENU_SHELL(menu),
            open);

        GtkWidget *edit =
            dock_menu_item_new(
                "Edit Launcher",
                "document-edit");

        g_object_set_data(
            G_OBJECT(edit),
            "dock-launcher-source",
            widget);

        g_signal_connect(
            edit,
            "activate",
            G_CALLBACK(
                dock_edit_launcher_menu_item_activated),
            dock);

        gtk_menu_shell_append(
            GTK_MENU_SHELL(menu),
            edit);

        if (dock->x11) {
            dock_x11_append_window_menu(
                dock->x11,
                widget,
                GTK_MENU_SHELL(menu));
        }

        GtkWidget *move =
            dock_menu_item_new(
            "Move to Drawer",
            "go-down");

        GtkWidget *move_submenu =
            gtk_menu_new();

        gtk_menu_set_reserve_toggle_size(
            GTK_MENU(move_submenu),
            FALSE);

        dock_build_move_to_drawer_submenu(
            GTK_MENU(move_submenu),
            dock,
            item);

        gtk_menu_item_set_submenu(
            GTK_MENU_ITEM(move),
            move_submenu);

        gtk_menu_shell_append(
            GTK_MENU_SHELL(menu),
            move);

        GtkWidget *remove =
            dock_menu_item_new(
            "Remove from Dock",
            "user-trash");

        g_object_set_data(
            G_OBJECT(remove),
            "dock-item",
            item);

        g_signal_connect(
            remove,
            "activate",
            G_CALLBACK(dock_remove_menu_item),
            dock);

        gtk_menu_shell_append(
            GTK_MENU_SHELL(menu),
            remove);

        gtk_menu_shell_append(
            GTK_MENU_SHELL(menu),
            gtk_separator_menu_item_new());
    }

    GtkWidget *preferences =
        dock_menu_item_new(
            "Preferences...",
            "preferences-system");

    g_signal_connect(
        preferences,
        "activate",
        G_CALLBACK(
            dock_preferences_menu_item_activated),
        dock);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        preferences);

    dock_append_monitor_menu(
        GTK_MENU_SHELL(menu),
        dock);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    GtkWidget *swap_side =
        dock_menu_item_new(
            "Swap Dock Side",
            "object-flip-horizontal");

    g_signal_connect(
        swap_side,
        "activate",
        G_CALLBACK(
            dock_swap_side_menu_item_activated),
        dock);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        swap_side);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    GtkWidget *add_drawer =
        dock_menu_item_new(
            "Add Drawer",
            "folder-new");

    g_signal_connect(
        add_drawer,
        "activate",
        G_CALLBACK(dock_add_drawer_menu_item_activated),
        dock);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        add_drawer);

    GtkWidget *add =
        dock_menu_item_new(
            "Add Application",
            "list-add");

    g_signal_connect(
        add,
        "activate",
        G_CALLBACK(
            dock_add_application_menu_item_activated),
        dock);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        add);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    dock_append_about_menu_item(
        GTK_MENU_SHELL(menu),
        dock);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    GtkWidget *quit =
        dock_menu_item_new(
            "Quit Crepido",
            "application-exit");

    g_signal_connect(
        quit,
        "activate",
        G_CALLBACK(dock_quit_menu_item_activated),
        dock);

    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        quit);

    gtk_widget_show_all(menu);

    dock_popup_menu_on_monitor(
        GTK_MENU(menu),
        widget,
        event,
        dock);

    return TRUE;
}

static gboolean
dock_point_inside(
    Dock *dock,
    gint root_x,
    gint root_y,
    gint *slot_out)
{
    if (!dock ||
        !dock->box)
        return FALSE;

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(
            dock->box);

    if (!toplevel ||
        !GTK_IS_WINDOW(toplevel))
        return FALSE;

    gint box_x = 0;
    gint box_y = 0;

    if (!gtk_widget_translate_coordinates(
            dock->box,
            toplevel,
            0,
            0,
            &box_x,
            &box_y))
        return FALSE;

    gint window_x = 0;
    gint window_y = 0;

    gtk_window_get_position(
        GTK_WINDOW(toplevel),
        &window_x,
        &window_y);

    GtkAllocation allocation;
    gtk_widget_get_allocation(
        dock->box,
        &allocation);

    gint x =
        root_x - window_x - box_x;
    gint y =
        root_y - window_y - box_y;

    if (x < 0 ||
        y < 0 ||
        x >= allocation.width ||
        y >= allocation.height)
        return FALSE;

    if (slot_out) {
        gint count =
            (gint)dock->items->len;

        *slot_out =
            CLAMP(
                (gint)floor(
                    (y + DOCK_TILE_SIZE / 2.0) /
                    DOCK_TILE_SIZE),
                0,
                MAX(0, count));
    }

    return TRUE;
}

static gboolean
dock_drawer_transfer_to_dock(
    DockDrawer *drawer,
    DockDrawerEntry *entry,
    gint target_index)
{
    if (!drawer ||
        !drawer->dock ||
        !entry ||
        !entry->desktop_id)
        return FALSE;

    Dock *dock = drawer->dock;

    if (dock_has_desktop_id(
            dock,
            entry->desktop_id))
        return FALSE;

    if (!dock_add_desktop_id(
            dock,
            entry->desktop_id))
        return FALSE;

    DockItem *new_item = NULL;

    for (guint i = 0;
         i < dock->items->len;
         i++) {
        DockItem *item =
            g_ptr_array_index(
                dock->items,
                i);

        if (!item->is_drawer &&
            g_strcmp0(
                item->desktop_id,
                entry->desktop_id) == 0) {
            new_item = item;
            break;
        }
    }

    if (new_item) {
        gint source_index =
            dock_find_item_index(
                dock,
                new_item);

        gint count =
            (gint)dock->items->len;

        gint clamped_target =
            CLAMP(
                target_index,
                0,
                MAX(0, count - 1));

        if (source_index >= 0 &&
            source_index != clamped_target) {
            DockItem *moved =
                g_ptr_array_steal_index(
                    dock->items,
                    (guint)source_index);

            g_ptr_array_insert(
                dock->items,
                (guint)clamped_target,
                moved);
        }

        dock_compact_items(dock);
    }

    guint drawer_index = G_MAXUINT;

    for (guint i = 0;
         i < drawer->entries->len;
         i++) {
        if (g_ptr_array_index(
                drawer->entries,
                i) == entry) {
            drawer_index = i;
            break;
        }
    }

    if (drawer_index == G_MAXUINT)
        return TRUE;

    g_ptr_array_remove_index(
        drawer->entries,
        drawer_index);

    if (drawer->popup)
        dock_drawer_open(drawer);

    dock_save(dock);

    return TRUE;
}

static void
dock_drag_proxy_destroy(
    Dock *dock)
{
    if (!dock)
        return;

    if (dock->drag_proxy) {
        gtk_widget_destroy(
            dock->drag_proxy);
        dock->drag_proxy = NULL;
    }

    dock->drag_proxy_visible = FALSE;
    dock->drag_proxy_x = 0.0;
    dock->drag_proxy_y = 0.0;
    dock->drag_proxy_target_x = 0.0;
    dock->drag_proxy_target_y = 0.0;
}

static void
dock_drag_proxy_update(
    Dock *dock,
    gboolean visible,
    gdouble root_x,
    gdouble root_y)
{
    if (!dock ||
        !dock->drag_item ||
        dock->drag_item->is_drawer ||
        !dock->drag_item->launcher)
        return;

    if (!dock->drag_proxy) {
        GtkWidget *popup =
            gtk_window_new(
                GTK_WINDOW_POPUP);

        gtk_window_set_decorated(
            GTK_WINDOW(popup),
            FALSE);
        gtk_window_set_resizable(
            GTK_WINDOW(popup),
            FALSE);
        gtk_window_set_skip_taskbar_hint(
            GTK_WINDOW(popup),
            TRUE);
        gtk_window_set_skip_pager_hint(
            GTK_WINDOW(popup),
            TRUE);
        gtk_window_set_type_hint(
            GTK_WINDOW(popup),
            GDK_WINDOW_TYPE_HINT_DOCK);
        gtk_window_set_accept_focus(
            GTK_WINDOW(popup),
            FALSE);
        gtk_window_set_focus_on_map(
            GTK_WINDOW(popup),
            FALSE);
        gtk_widget_set_opacity(
            popup,
            dock->opacity_percent / 100.0);

        GtkWidget *button =
            dock_icon_button_new(
                dock->drag_item->launcher);

        if (!button) {
            gtk_widget_destroy(popup);
            return;
        }

        dock_icon_button_set_icon_size(
            button,
            dock->icon_size);

        gtk_widget_set_sensitive(
            button,
            FALSE);
        gtk_widget_set_focus_on_click(
            button,
            FALSE);
        gtk_widget_set_size_request(
            button,
            DOCK_TILE_SIZE,
            DOCK_TILE_SIZE);

        gtk_container_add(
            GTK_CONTAINER(popup),
            button);

        dock->drag_proxy = popup;
        dock->drag_proxy_x =
            root_x - DOCK_TILE_SIZE / 2.0;
        dock->drag_proxy_y =
            root_y - DOCK_TILE_SIZE / 2.0;
        dock->drag_proxy_target_x =
            dock->drag_proxy_x;
        dock->drag_proxy_target_y =
            dock->drag_proxy_y;
    }

    dock->drag_proxy_target_x =
        root_x - DOCK_TILE_SIZE / 2.0;
    dock->drag_proxy_target_y =
        root_y - DOCK_TILE_SIZE / 2.0;

    if (visible) {
        if (!dock->drag_proxy_visible) {
            dock->drag_proxy_visible = TRUE;
            gtk_widget_show_all(
                dock->drag_proxy);
        }
    } else if (dock->drag_proxy_visible) {
        dock->drag_proxy_visible = FALSE;
        gtk_widget_hide(
            dock->drag_proxy);
    }
}

static void
dock_begin_drag(
    Dock *dock,
    GtkWidget *button)
{
    if (dock->dragging ||
        dock->dropping ||
        !dock->press_active)
        return;

    DockItem *item =
        g_object_get_data(
            G_OBJECT(button),
            "dock-item");

    if (!item)
        return;

    gint source_index =
        dock_find_item_index(
            dock,
            item);

    if (source_index < 0)
        return;

    if (item->is_drawer)
        dock_drawer_close(item->drawer);

    /*
     * A grouped-instance popup is a separate animated X11 window. It must
     * not keep moving while the Dock's own drag/reorder animation is active.
     */
    if (dock->x11) {
        dock_x11_close_group_popup(dock->x11);
        dock_x11_set_group_popup_suppressed(
            dock->x11,
            TRUE);
    }

    dock->press_group_popup_state = 0;

    dock->drag_hover_drawer = NULL;
    dock->drag_drop_drawer = NULL;
    dock->drag_drop_slot = -1;
    dock->drag_drop_on_tile = FALSE;

    dock->dragging = TRUE;
    dock->dropping = FALSE;
    dock->drag_item = item;
    dock->drag_button = button;
    dock->source_index = source_index;
    dock->target_index = source_index;
    dock->drag_target_y =
        source_index * DOCK_TILE_SIZE;
    dock->last_frame_us = 0;

    dock_set_dragging_state(
        button,
        TRUE);

    g_object_set_data(
        G_OBJECT(button),
        "dock-suppress-click",
        GINT_TO_POINTER(TRUE));

    dock_reset_all_targets(dock);
    dock_update_reorder_indicator(dock);

    /*
     * Keep receiving motion/release events after the pointer crosses
     * from the main Dock into an open Drawer popup.
     */
    gtk_grab_add(button);

    dock_ensure_tick(dock);
}

static void
dock_update_drag(
    Dock *dock,
    gdouble root_x,
    gdouble root_y)
{
    if (!dock->dragging ||
        !dock->drag_item ||
        dock->dropping)
        return;

    gint count =
        (gint)dock->items->len;

    if (count <= 0)
        return;

    /*
     * Keep the Dock's vertical layout fixed once the pointer leaves the
     * Dock strip. This prevents an open Drawer tile from being shifted
     * while the dragged application crosses into the Drawer popup.
     */
    if (!dock_point_inside(
            dock,
            (gint)root_x,
            (gint)root_y,
            NULL)) {
        dock_update_reorder_indicator(dock);
        dock_ensure_tick(dock);
        return;
    }

    gdouble offset_y =
        root_y - dock->press_root_y;

    gdouble drag_y =
        dock->source_index * DOCK_TILE_SIZE +
        offset_y;

    dock->drag_target_y =
        CLAMP(
            drag_y,
            0.0,
            (gdouble)((count - 1) * DOCK_TILE_SIZE));

    gint target_index =
        CLAMP(
            (gint)floor(
                (dock->drag_target_y +
                 DOCK_TILE_SIZE / 2.0) /
                DOCK_TILE_SIZE),
            0,
            count - 1);

    if (target_index != dock->target_index) {
        dock->target_index = target_index;

        dock_set_neighbor_targets(
            dock,
            dock->source_index,
            target_index);
    }

    dock_update_reorder_indicator(dock);
    dock_ensure_tick(dock);
}

static void
dock_end_drag(Dock *dock)
{
    if (!dock->dragging ||
        !dock->drag_item)
        return;

    if (dock->drag_button)
        gtk_grab_remove(
            dock->drag_button);

    dock_drag_proxy_destroy(dock);

    dock_reset_drag_drawer_state(dock);

    gint source_index =
        dock->source_index;

    gint target_index =
        dock->target_index;

    if (source_index >= 0 &&
        target_index >= 0 &&
        source_index != target_index) {
        DockItem *item =
            g_ptr_array_steal_index(
                dock->items,
                (guint)source_index);

        g_ptr_array_insert(
            dock->items,
            (guint)target_index,
            item);
    }

    dock_set_final_targets(dock);

    if (source_index != target_index)
        dock_save(dock);

    dock->dropping = TRUE;
    dock_ensure_tick(dock);
}

static gboolean
dock_drawer_tile_point_inside(
    DockDrawer *drawer,
    gint root_x,
    gint root_y)
{
    if (!drawer ||
        !drawer->button)
        return FALSE;

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(
            drawer->button);

    if (!toplevel ||
        !GTK_IS_WINDOW(toplevel))
        return FALSE;

    gint box_x = 0;
    gint box_y = 0;

    if (!gtk_widget_translate_coordinates(
            drawer->dock->box,
            toplevel,
            0,
            0,
            &box_x,
            &box_y))
        return FALSE;

    gint window_x = 0;
    gint window_y = 0;

    gtk_window_get_position(
        GTK_WINDOW(toplevel),
        &window_x,
        &window_y);

    GtkAllocation allocation;
    gtk_widget_get_allocation(
        drawer->button,
        &allocation);

    gint x =
        window_x + box_x + allocation.x;
    gint y =
        window_y + box_y + allocation.y;

    return root_x >= x &&
           root_x < x + allocation.width &&
           root_y >= y &&
           root_y < y + allocation.height;
}

static DockDrawer *
dock_find_drawer_drop_target(
    Dock *dock,
    gint root_x,
    gint root_y,
    gint *target_index_out)
{
    if (!dock)
        return NULL;

    /*
     * Treat each visible Drawer application button as a concrete drop
     * target. This makes dropping directly on an icon reliable instead of
     * depending only on the popup window's background hit rectangle.
     */
    for (guint i = 0;
         i < dock->items->len;
         i++) {
        DockItem *item =
            g_ptr_array_index(
                dock->items,
                i);

        DockDrawer *drawer =
            item->is_drawer ?
            item->drawer :
            NULL;

        if (!drawer ||
            !drawer->popup ||
            drawer->animation_closing)
            continue;

        GdkWindow *popup_window =
            gtk_widget_get_window(
                drawer->popup);

        if (!popup_window)
            continue;

        gint popup_x = 0;
        gint popup_y = 0;

        gdk_window_get_origin(
            popup_window,
            &popup_x,
            &popup_y);

        GtkWidget *container =
            gtk_bin_get_child(
                GTK_BIN(drawer->popup));

        if (!container ||
            !GTK_IS_CONTAINER(container))
            continue;

        GList *children =
            gtk_container_get_children(
                GTK_CONTAINER(container));

        gint entry_index = 0;
        DockDrawer *hit_drawer = NULL;
        gint hit_index = -1;

        for (GList *iter = children;
             iter;
             iter = iter->next) {
            GtkWidget *child =
                GTK_WIDGET(iter->data);

            if (!g_object_get_data(
                    G_OBJECT(child),
                    "dock-drawer-entry"))
                continue;

            GtkAllocation allocation;
            gtk_widget_get_allocation(
                child,
                &allocation);

            gint x =
                popup_x + allocation.x;
            gint y =
                popup_y + allocation.y;

            if (root_x >= x &&
                root_x < x + allocation.width &&
                root_y >= y &&
                root_y < y + allocation.height) {
                hit_drawer = drawer;
                hit_index = entry_index;
                break;
            }

            entry_index++;
        }

        g_list_free(children);

        if (hit_drawer) {
            if (target_index_out)
                *target_index_out = hit_index;

            return hit_drawer;
        }
    }

    /*
     * Also recognize the popup background/empty slot. When the pointer is
     * not directly over an icon, calculate the nearest insertion slot.
     */
    for (guint i = 0;
         i < dock->items->len;
         i++) {
        DockItem *item =
            g_ptr_array_index(
                dock->items,
                i);

        if (!item->is_drawer ||
            !item->drawer ||
            !item->drawer->popup ||
            item->drawer->animation_closing)
            continue;

        GdkWindow *popup_window =
            gtk_widget_get_window(
                item->drawer->popup);

        if (!popup_window)
            continue;

        gint popup_x = 0;
        gint popup_y = 0;

        gdk_window_get_origin(
            popup_window,
            &popup_x,
            &popup_y);

        GtkAllocation allocation;
        gtk_widget_get_allocation(
            item->drawer->popup,
            &allocation);

        if (root_x < popup_x ||
            root_y < popup_y ||
            root_x >= popup_x + allocation.width ||
            root_y >= popup_y + allocation.height)
            continue;

        if (target_index_out) {
            gint count =
                (gint)item->drawer->entries->len;

            gint x =
                root_x - popup_x;

            *target_index_out =
                CLAMP(
                    (gint)floor(
                        (x + DOCK_TILE_SIZE / 2.0) /
                        DOCK_TILE_SIZE),
                    0,
                    count);
        }

        return item->drawer;
    }

    /*
     * The Drawer tile itself is also a valid drop target. This is critical
     * while its popup is still opening, and lets a drop succeed even when
     * the pointer never enters the expanded area.
     */
    for (guint i = 0;
         i < dock->items->len;
         i++) {
        DockItem *item =
            g_ptr_array_index(
                dock->items,
                i);

        if (!item->is_drawer ||
            !item->drawer ||
            item->drawer->animation_closing)
            continue;

        if (dock_drawer_tile_point_inside(
                item->drawer,
                root_x,
                root_y)) {
            if (target_index_out)
                *target_index_out =
                    (gint)item->drawer->entries->len;

            return item->drawer;
        }
    }

    return NULL;
}


static DockDrawer *
dock_find_drawer_tile_at_point(
    Dock *dock,
    gint root_x,
    gint root_y)
{
    if (!dock)
        return NULL;

    for (guint i = 0;
         i < dock->items->len;
         i++) {
        DockItem *item =
            g_ptr_array_index(
                dock->items,
                i);

        if (!item->is_drawer ||
            !item->drawer)
            continue;

        if (dock_drawer_tile_point_inside(
                item->drawer,
                root_x,
                root_y))
            return item->drawer;
    }

    return NULL;
}

static void
dock_drawer_clear_drop_states(
    Dock *dock)
{
    if (!dock)
        return;

    for (guint i = 0;
         i < dock->items->len;
         i++) {
        DockItem *item =
            g_ptr_array_index(
                dock->items,
                i);

        if (!item->is_drawer ||
            !item->drawer)
            continue;

        gtk_widget_unset_state_flags(
            item->drawer->button,
            GTK_STATE_FLAG_DROP_ACTIVE);

        if (!item->drawer->popup)
            continue;

        GtkWidget *container =
            gtk_bin_get_child(
                GTK_BIN(item->drawer->popup));

        if (!container ||
            !GTK_IS_CONTAINER(container))
            continue;

        GList *children =
            gtk_container_get_children(
                GTK_CONTAINER(container));

        for (GList *iter = children;
             iter;
             iter = iter->next) {
            gtk_widget_unset_state_flags(
                GTK_WIDGET(iter->data),
                GTK_STATE_FLAG_DROP_ACTIVE);
        }

        g_list_free(children);
    }
}

static void
dock_drawer_update_drop_states(
    Dock *dock,
    gint root_x,
    gint root_y)
{
    if (!dock)
        return;

    DockDrawer *tile_target =
        dock_find_drawer_tile_at_point(
            dock,
            root_x,
            root_y);

    gint popup_target_index = -1;

    DockDrawer *popup_target =
        dock_find_drawer_drop_target(
            dock,
            root_x,
            root_y,
            &popup_target_index);

    DockDrawer *target_drawer = NULL;
    gint target_slot = -1;
    gboolean on_tile = FALSE;

    if (tile_target &&
        !tile_target->animation_closing &&
        (!tile_target->popup ||
         !popup_target)) {
        target_drawer = tile_target;
        on_tile = TRUE;
    } else if (popup_target &&
               popup_target->popup) {
        target_drawer = popup_target;
        target_slot = popup_target_index;
    }

    if (target_drawer == dock->drag_drop_drawer &&
        target_slot == dock->drag_drop_slot &&
        on_tile == dock->drag_drop_on_tile)
        return;

    dock_drawer_clear_drop_states(dock);

    dock->drag_drop_drawer = target_drawer;
    dock->drag_drop_slot = target_slot;
    dock->drag_drop_on_tile = on_tile;

    if (!target_drawer)
        return;

    if (on_tile) {
        gtk_widget_set_state_flags(
            target_drawer->button,
            GTK_STATE_FLAG_DROP_ACTIVE,
            TRUE);
        return;
    }

    if (!target_drawer->popup)
        return;

    GtkWidget *container =
        gtk_bin_get_child(
            GTK_BIN(target_drawer->popup));

    if (!container ||
        !GTK_IS_CONTAINER(container))
        return;

    GList *children =
        gtk_container_get_children(
            GTK_CONTAINER(container));

    GtkWidget *highlight = NULL;
    gint entry_index = 0;

    for (GList *iter = children;
         iter;
         iter = iter->next) {
        GtkWidget *child =
            GTK_WIDGET(iter->data);

        if (!g_object_get_data(
                G_OBJECT(child),
                "dock-drawer-entry"))
            continue;

        if (entry_index == target_slot) {
            highlight = child;
            break;
        }

        entry_index++;
    }

    if (!highlight &&
        entry_index > 0) {
        for (GList *iter = children;
             iter;
             iter = iter->next) {
            GtkWidget *child =
                GTK_WIDGET(iter->data);

            if (g_object_get_data(
                    G_OBJECT(child),
                    "dock-drawer-entry")) {
                highlight = child;
            }
        }
    }

    if (highlight) {
        gtk_widget_set_state_flags(
            highlight,
            GTK_STATE_FLAG_DROP_ACTIVE,
            TRUE);
    }

    g_list_free(children);
}

static void
dock_reset_drag_drawer_state(
    Dock *dock)
{
    if (!dock)
        return;

    if (dock->drag_hover_drawer &&
        dock->drag_hover_drawer->hover_open_id) {
        g_source_remove(
            dock->drag_hover_drawer->hover_open_id);
        dock->drag_hover_drawer->hover_open_id = 0;
    }

    dock->drag_hover_drawer = NULL;
    dock->drag_drop_drawer = NULL;
    dock->drag_drop_slot = -1;
    dock->drag_drop_on_tile = FALSE;
    dock->drawer_drop_on_dock = FALSE;
    dock->drawer_drop_dock_slot = -1;

    dock_update_drawer_drop_indicator(
        dock,
        -1);

    dock_drawer_clear_drop_states(dock);
}



static void
dock_update_drag_drawer_hover(
    Dock *dock,
    gint root_x,
    gint root_y)
{
    DockDrawer *tile_target =
        dock_find_drawer_tile_at_point(
            dock,
            root_x,
            root_y);

    DockDrawer *popup_target =
        dock_find_drawer_drop_target(
            dock,
            root_x,
            root_y,
            NULL);

    DockDrawer *hover_target =
        tile_target ?
        tile_target :
        popup_target;

    if (hover_target != dock->drag_hover_drawer) {
        if (dock->drag_hover_drawer &&
            dock->drag_hover_drawer->hover_open_id) {
            g_source_remove(
                dock->drag_hover_drawer->hover_open_id);
            dock->drag_hover_drawer->hover_open_id = 0;
        }

        dock->drag_hover_drawer = hover_target;

        if (hover_target &&
            !hover_target->popup &&
            !hover_target->animation_closing) {
            hover_target->hover_open_id =
                g_timeout_add(
                    DRAWER_DRAG_HOVER_OPEN_DELAY_MS,
                    dock_drawer_hover_open,
                    hover_target);
        }
    }

    dock_drawer_update_drop_states(
        dock,
        root_x,
        root_y);
}


static gboolean
dock_transfer_to_drawer(
    Dock *dock,
    DockItem *item,
    DockDrawer *drawer,
    gint target_index)
{
    if (!dock ||
        !item ||
        item->is_drawer ||
        !drawer ||
        !item->desktop_id)
        return FALSE;

    if (dock_count_application_items(dock) <= 1)
        return FALSE;

    if (dock_drawer_has_desktop_id(
            drawer,
            item->desktop_id))
        return FALSE;

    gint source_index =
        dock_find_item_index(
            dock,
            item);

    if (source_index < 0)
        return FALSE;

    DockLauncher *launcher =
        dock_launcher_new_from_desktop_id(
            item->desktop_id);

    if (!launcher)
        return FALSE;

    DockDrawerEntry *entry =
        g_new0(DockDrawerEntry, 1);

    entry->desktop_id =
        g_strdup(item->desktop_id);
    entry->launcher =
        launcher;

    gboolean was_open =
        drawer->popup != NULL;

    gint count =
        (gint)drawer->entries->len;

    gint insert_index =
        CLAMP(
            target_index,
            0,
            count);

    g_ptr_array_insert(
        drawer->entries,
        (guint)insert_index,
        entry);

    /*
     * A Drawer may have opened while the pointer hovered it during the
     * drag. Do not leave that transient popup visible after the transfer:
     * the application belongs inside the Drawer and should remain hidden
     * until the Drawer is opened normally again. Destroying the popup now
     * also prevents it from retaining the Drawer's old screen position
     * while the main Dock compacts.
     */
    if (was_open)
        dock_drawer_destroy_popup(drawer);

    DockItem *removed =
        g_ptr_array_steal_index(
            dock->items,
            (guint)source_index);

    gtk_widget_destroy(
        removed->button);
    dock_item_free(removed);

    dock_compact_items(dock);
    dock_save(dock);

    return TRUE;
}

static gboolean
dock_button_press(
    GtkWidget *widget,
    GdkEventButton *event,
    gpointer user_data)
{
    Dock *dock = user_data;

    if (event->button == GDK_BUTTON_SECONDARY) {
        return dock_popup_menu(
            widget,
            event,
            dock);
    }

    if (event->button == GDK_BUTTON_MIDDLE)
        return TRUE;

    if (event->button != GDK_BUTTON_PRIMARY)
        return FALSE;

    dock->press_group_popup_state =
        dock->x11 ?
        dock_x11_prepare_group_popup_for_press(
            dock->x11,
            widget) :
        0;

    if (dock->dragging ||
        dock->dropping)
        return TRUE;

    dock->press_active = TRUE;
    dock->press_button = widget;
    dock->press_root_x = event->x_root;
    dock->press_root_y = event->y_root;
    dock->press_active_window =
        dock->x11 ?
        (guint64)dock_x11_get_active_window(dock->x11) :
        0;

    g_object_set_data(
        G_OBJECT(widget),
        "dock-suppress-click",
        GINT_TO_POINTER(FALSE));

    /*
     * Let GtkButton render its normal pressed state. We take over the
     * release only after deciding whether this was a click or a drag.
     */
    return FALSE;
}

static gboolean
dock_button_motion(
    GtkWidget *widget,
    GdkEventMotion *event,
    gpointer user_data)
{
    Dock *dock = user_data;

    if (!dock->press_active ||
        dock->press_button != widget ||
        dock->dropping)
        return FALSE;

    gdouble distance =
        hypot(
            event->x_root - dock->press_root_x,
            event->y_root - dock->press_root_y);

    if (!dock->dragging &&
        distance >= 5.0) {
        dock_begin_drag(
            dock,
            widget);
    }

    if (dock->dragging) {
        gint root_x = (gint)event->x_root;
        gint root_y = (gint)event->y_root;
        gboolean inside_dock =
            dock_point_inside(
                dock,
                root_x,
                root_y,
                NULL);

        dock_update_drag_drawer_hover(
            dock,
            root_x,
            root_y);

        dock_drag_proxy_update(
            dock,
            !inside_dock,
            root_x,
            root_y);

        if (!dock_find_drawer_tile_at_point(
                dock,
                root_x,
                root_y)) {
            dock_update_drag(
                dock,
                event->x_root,
                event->y_root);
        } else {
            dock_ensure_tick(dock);
        }
    }

    return FALSE;
}

static gboolean
dock_button_release(
    GtkWidget *widget,
    GdkEventButton *event,
    gpointer user_data)
{
    Dock *dock = user_data;

    if (event->button != GDK_BUTTON_PRIMARY)
        return FALSE;

    if (dock->dragging &&
        dock->drag_button == widget) {
        dock->press_active = FALSE;
        dock->press_button = NULL;

        DockItem *item =
            g_object_get_data(
                G_OBJECT(widget),
                "dock-item");

        gint target_index = -1;

        DockDrawer *target_drawer =
            dock_find_drawer_drop_target(
                dock,
                (gint)event->x_root,
                (gint)event->y_root,
                &target_index);

        if (target_drawer &&
            item &&
            !item->is_drawer &&
            dock_count_application_items(dock) > 1) {
            gtk_grab_remove(widget);

            dock_drag_proxy_destroy(dock);

            if (dock_transfer_to_drawer(
                    dock,
                    item,
                    target_drawer,
                    target_index)) {
                if (dock->tick_id) {
                    gtk_widget_remove_tick_callback(
                        dock->box,
                        dock->tick_id);
                    dock->tick_id = 0;
                }

                dock_set_dragging_state(
                    widget,
                    FALSE);

                dock_drag_proxy_destroy(dock);

                dock_drawer_clear_drop_states(dock);

                if (dock->x11)
                    dock_x11_set_group_popup_suppressed(
                        dock->x11,
                        FALSE);

                dock_reset_drag_drawer_state(dock);

                dock->press_group_popup_state = 0;
                dock->dragging = FALSE;
                dock->dropping = FALSE;
                dock->drag_item = NULL;
                dock->drag_button = NULL;
                dock->source_index = -1;
                dock->target_index = -1;

                return TRUE;
            }
        }

        /*
         * Match Crepido's core Dock behavior: dragging an application
         * icon completely outside the Dock removes it. Keep one normal
         * launcher as a safety floor because this standalone Dock uses the
         * first launcher as its fallback when repairing a damaged config.
         */
        if (item &&
            !item->is_drawer &&
            dock_count_application_items(dock) > 1 &&
            !dock_point_inside(
                dock,
                (gint)event->x_root,
                (gint)event->y_root,
                NULL)) {
            gint source_index =
                dock->source_index;

            gtk_grab_remove(widget);

            if (dock->tick_id) {
                gtk_widget_remove_tick_callback(
                    dock->box,
                    dock->tick_id);
                dock->tick_id = 0;
            }

            dock_drag_proxy_destroy(dock);
            dock_reset_drag_drawer_state(dock);

            if (source_index >= 0 &&
                source_index < (gint)dock->items->len) {
                DockItem *removed =
                    g_ptr_array_steal_index(
                        dock->items,
                        (guint)source_index);

                gtk_widget_destroy(
                    removed->button);
                dock_item_free(removed);
            }

            dock->dragging = FALSE;
            dock->dropping = FALSE;
            dock->drag_item = NULL;
            dock->drag_button = NULL;
            dock->press_active = FALSE;
            dock->press_button = NULL;
            dock->source_index = -1;
            dock->target_index = -1;

            dock_compact_items(dock);
            dock_save(dock);

            return TRUE;
        }

        dock_end_drag(dock);

        return FALSE;
    }

    if (dock->press_active &&
        dock->press_button == widget) {
        gint press_group_popup_state =
            dock->press_group_popup_state;

        dock->press_group_popup_state = 0;
        dock->press_active = FALSE;
        dock->press_button = NULL;

        g_object_set_data(
            G_OBJECT(widget),
            "dock-suppress-click",
            GINT_TO_POINTER(TRUE));

        DockItem *item =
            g_object_get_data(
                G_OBJECT(widget),
                "dock-item");

        if (item && item->is_drawer) {
            dock_drawer_toggle(item->drawer);
            return FALSE;
        }

        DockLauncher *launcher =
            g_object_get_data(
                G_OBJECT(widget),
                "dock-launcher");

        if (launcher &&
            press_group_popup_state == 2) {
            return FALSE;
        }

        if (launcher) {
            gboolean activated = FALSE;
            GError *error = NULL;

            if (dock->x11) {
                activated =
                    dock_x11_activate_button(
                        dock->x11,
                        widget,
                        event->time,
                        (Window)dock->press_active_window);
            }

            if (!activated) {
                if (!dock_launcher_launch(
                        launcher,
                        widget,
                        event->time,
                        &error)) {
                    g_warning(
                        "Unable to launch Dock application '%s': %s",
                        dock_launcher_get_name(launcher),
                        error ? error->message : "unknown error");
                    g_clear_error(&error);
                } else {
                    dock_icon_button_set_launching(
                        widget,
                        TRUE);
                }
            }
        }

        return FALSE;
    }

    return FALSE;
}

static void
dock_item_free(gpointer data)
{
    DockItem *item = data;

    if (!item)
        return;

    g_clear_pointer(
        &item->desktop_id,
        g_free);

    if (item->is_drawer)
        dock_drawer_free(item->drawer);
    else
        dock_launcher_free(item->launcher);

    g_free(item);
}

Dock *
dock_new(void)
{
    Dock *dock =
        g_new0(Dock, 1);

    dock->items =
        g_ptr_array_new_with_free_func(
            dock_item_free);

    dock->source_index = -1;
    dock->target_index = -1;
    dock->on_right_side = TRUE;
    dock->position_mode = DOCK_POSITION_NORMAL;
    dock->monitor_index = 0;
    dock->icon_size = 48;
    dock->opacity_percent = 100;
    dock->bitmap_background_mode = DOCK_BITMAP_BACKGROUND_REPEAT;
    dock->show_window_indicator = TRUE;
    dock->show_hide_handle = TRUE;
    dock->shortcut_only = FALSE;
    dock->minimized_window_icons = FALSE;
    dock->minimized_title_labels = FALSE;
    dock->minimized_group_drawer = FALSE;
    dock->minimized_all_workspaces = FALSE;
    dock->drawer_drop_dock_slot = -1;

    dock->box =
        gtk_fixed_new();

    GdkDisplay *default_display =
        gdk_display_get_default();

    if (default_display) {
        GdkMonitor *primary_monitor =
            gdk_display_get_primary_monitor(
                default_display);

        gint monitor_count =
            gdk_display_get_n_monitors(
                default_display);

        for (gint i = 0;
             i < monitor_count;
             i++) {
            if (gdk_display_get_monitor(
                    default_display,
                    i) == primary_monitor) {
                dock->monitor_index = i;
                break;
            }
        }
        
        dock->monitor_display =
            g_object_ref(default_display);

        GdkScreen *screen =
            gdk_display_get_default_screen(default_display);

        if (screen)
            dock->monitor_screen = g_object_ref(screen);

        GdkMonitor *selected_monitor =
            gdk_display_get_monitor(
                default_display,
                dock->monitor_index);
        dock_monitor_cache_selected(
            dock,
            selected_monitor);

        dock->monitor_added_handler =
            g_signal_connect(
                default_display,
                "monitor-added",
                G_CALLBACK(dock_display_monitor_changed),
                dock);
        dock->monitor_removed_handler =
            g_signal_connect(
                default_display,
                "monitor-removed",
                G_CALLBACK(dock_display_monitor_changed),
                dock);

        if (dock->monitor_screen) {
            dock->monitors_changed_handler =
                g_signal_connect(
                    dock->monitor_screen,
                    "monitors-changed",
                    G_CALLBACK(dock_screen_monitors_changed),
                    dock);
            dock->screen_size_changed_handler =
                g_signal_connect(
                    dock->monitor_screen,
                    "size-changed",
                    G_CALLBACK(dock_screen_monitors_changed),
                    dock);
        }
    }

    dock->x11 =
        dock_x11_new(dock->box);

    if (dock->x11) {
        dock_x11_set_icon_size(
            dock->x11,
            dock->icon_size);
        dock_x11_set_shortcut_only(
            dock->x11,
            dock->shortcut_only);
        dock_x11_set_minimized_window_icons(
            dock->x11,
            dock->minimized_window_icons);
        dock_x11_set_minimized_title_labels(
            dock->x11,
            dock->minimized_title_labels);
        dock_x11_set_minimized_group_drawer(
            dock->x11,
            dock->minimized_group_drawer);
        dock_x11_set_minimized_all_workspaces(
            dock->x11,
            dock->minimized_all_workspaces);
        dock_x11_set_opacity(
            dock->x11,
            dock->opacity_percent);
    }

    gtk_widget_set_size_request(
        dock->box,
        dock_get_window_width(dock),
        dock_get_window_height(dock));

    /*
     * Right-clicking any populated slot provides the Dock's context
     * menu, including Add Application and item-specific actions.
     */
    gtk_widget_add_events(
        dock->box,
        GDK_BUTTON_PRESS_MASK |
        GDK_ENTER_NOTIFY_MASK |
        GDK_LEAVE_NOTIFY_MASK);

    g_signal_connect(
        dock->box,
        "size-allocate",
        G_CALLBACK(dock_box_size_allocate),
        dock);

    g_signal_connect(
        dock->box,
        "button-press-event",
        G_CALLBACK(dock_popup_menu),
        dock);

    g_signal_connect(
        dock->box,
        "enter-notify-event",
        G_CALLBACK(dock_enter_notify),
        dock);

    g_signal_connect(
        dock->box,
        "leave-notify-event",
        G_CALLBACK(dock_leave_notify),
        dock);

    dock_enable_desktop_drop(
        dock->box,
        dock);

    dock_create_hide_handle(dock);

    return dock;
}

void
dock_free(Dock *dock)
{
    if (!dock)
        return;

    if (dock->monitor_refresh_id) {
        g_source_remove(dock->monitor_refresh_id);
        dock->monitor_refresh_id = 0;
    }

    if (dock->monitor_display) {
        if (dock->monitor_added_handler)
            g_signal_handler_disconnect(
                dock->monitor_display,
                dock->monitor_added_handler);
        if (dock->monitor_removed_handler)
            g_signal_handler_disconnect(
                dock->monitor_display,
                dock->monitor_removed_handler);
    }

    if (dock->monitor_screen) {
        if (dock->monitors_changed_handler)
            g_signal_handler_disconnect(
                dock->monitor_screen,
                dock->monitors_changed_handler);
        if (dock->screen_size_changed_handler)
            g_signal_handler_disconnect(
                dock->monitor_screen,
                dock->screen_size_changed_handler);
    }

    g_clear_object(&dock->monitor_object);
    g_clear_object(&dock->monitor_display);
    g_clear_object(&dock->monitor_screen);
    g_clear_pointer(&dock->monitor_manufacturer, g_free);
    g_clear_pointer(&dock->monitor_model, g_free);

    if (dock->tick_id) {
        gtk_widget_remove_tick_callback(
            dock->box,
            dock->tick_id);
        dock->tick_id = 0;
    }

    if (dock->hide_animation_tick_id) {
        g_source_remove(dock->hide_animation_tick_id);
        dock->hide_animation_tick_id = 0;
    }

    dock->hide_animating = FALSE;

    dock_drag_proxy_destroy(dock);
    dock_set_bitmap_background(
        dock,
        FALSE,
        NULL,
        dock->bitmap_background_mode);

    g_clear_pointer(
        &dock->items,
        g_ptr_array_unref);

    dock_x11_free(dock->x11);
    dock->x11 = NULL;

    if (dock->box) {
        gtk_widget_destroy(
            dock->box);
        dock->box = NULL;
    }

    g_free(dock);
}

gboolean
dock_add_desktop_id(
    Dock *dock,
    const gchar *desktop_id)
{
    g_return_val_if_fail(
        dock != NULL,
        FALSE);

    g_return_val_if_fail(
        desktop_id != NULL,
        FALSE);

    if (dock_has_desktop_id(
            dock,
            desktop_id))
        return FALSE;

    DockLauncher *launcher =
        dock_launcher_new_from_desktop_id(
            desktop_id);

    if (!launcher)
        return FALSE;

    GtkWidget *button =
        dock_icon_button_new(
            launcher);

    if (!button) {
        dock_launcher_free(launcher);
        return FALSE;
    }

    dock_icon_button_set_icon_size(
        button,
        dock->icon_size);

    DockItem *item =
        g_new0(DockItem, 1);

    item->desktop_id =
        g_strdup(desktop_id);
    item->launcher = launcher;
    item->button = button;

    guint index =
        dock->items->len;

    item->visual_y =
        index * DOCK_TILE_SIZE;
    item->target_y =
        item->visual_y;
    item->applied_y = -1;

    gtk_widget_set_size_request(
        button,
        DOCK_TILE_SIZE,
        DOCK_TILE_SIZE);

    gtk_fixed_put(
        GTK_FIXED(dock->box),
        button,
        dock_get_items_x(dock),
        (gint)item->visual_y);

    g_object_set_data(
        G_OBJECT(button),
        "dock-item",
        item);

    g_signal_connect_after(
        button,
        "draw",
        G_CALLBACK(dock_insertion_indicator_draw),
        NULL);

    dock_set_dragging_state(
        button,
        FALSE);

    gtk_widget_add_events(
        button,
        GDK_BUTTON_PRESS_MASK |
        GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK |
        GDK_ENTER_NOTIFY_MASK |
        GDK_LEAVE_NOTIFY_MASK);

    g_signal_connect(
        button,
        "enter-notify-event",
        G_CALLBACK(dock_enter_notify),
        dock);

    g_signal_connect(
        button,
        "leave-notify-event",
        G_CALLBACK(dock_leave_notify),
        dock);

    dock_enable_desktop_drop(
        button,
        dock);

    g_signal_connect(
        button,
        "button-press-event",
        G_CALLBACK(dock_button_press),
        dock);

    g_signal_connect(
        button,
        "motion-notify-event",
        G_CALLBACK(dock_button_motion),
        dock);

    g_signal_connect_after(
        button,
        "button-release-event",
        G_CALLBACK(dock_button_release),
        dock);

    /*
     * dock_add_desktop_id() can run after the Dock window has already been
     * shown. Explicitly show the new button so its themed child is visible.
     */
    gtk_widget_show_all(button);

    g_ptr_array_add(
        dock->items,
        item);

    dock_resize_to_items(dock);

    return TRUE;
}

gchar **
dock_dup_desktop_ids(
    const Dock *dock)
{
    g_return_val_if_fail(dock != NULL, NULL);

    gchar **ids =
        g_new0(gchar *, dock->items->len + 1);

    guint output = 0;

    for (guint i = 0; i < dock->items->len; i++) {
        DockItem *item =
            g_ptr_array_index(dock->items, i);

        if (item->is_drawer)
            continue;

        ids[output++] =
            g_strdup(item->desktop_id);
    }

    return ids;
}

guint
dock_get_application_count(
    const Dock *dock)
{
    g_return_val_if_fail(
        dock != NULL,
        0);

    return dock_count_application_items(dock);
}

guint
dock_get_item_count(
    const Dock *dock)
{
    g_return_val_if_fail(
        dock != NULL,
        0);

    return dock->items->len;
}

void
dock_set_right_side(
    Dock *dock,
    gboolean on_right_side)
{
    g_return_if_fail(dock != NULL);

    if (dock->on_right_side == !!on_right_side)
        return;

    GPtrArray *open_drawers =
        g_ptr_array_new();

    for (guint i = 0;
         i < dock->items->len;
         i++) {
        DockItem *item =
            g_ptr_array_index(
                dock->items,
                i);

        if (!item->is_drawer ||
            !item->drawer ||
            !item->drawer->popup)
            continue;

        g_ptr_array_add(
            open_drawers,
            item->drawer);

        dock_drawer_destroy_popup(
            item->drawer);
    }

    dock->on_right_side = !!on_right_side;

    dock_compact_items(dock);
    dock_position_window(dock);

    for (guint i = 0;
         i < open_drawers->len;
         i++) {
        DockDrawer *drawer =
            g_ptr_array_index(
                open_drawers,
                i);

        dock_drawer_open(drawer);
    }

    g_ptr_array_free(
        open_drawers,
        TRUE);
}


void
dock_set_show_window_indicator(
    Dock *dock,
    gboolean enabled)
{
    g_return_if_fail(dock != NULL);

    dock->show_window_indicator = !!enabled;

    if (dock->x11)
        dock_x11_set_window_indicator_enabled(
            dock->x11,
            dock->show_window_indicator);
}

gboolean
dock_get_show_window_indicator(
    const Dock *dock)
{
    g_return_val_if_fail(
        dock != NULL,
        TRUE);

    return dock->show_window_indicator;
}

void
dock_set_show_hide_handle(
    Dock *dock,
    gboolean enabled)
{
    g_return_if_fail(dock != NULL);

    enabled = !!enabled;

    if (dock->show_hide_handle == enabled)
        return;

    dock->show_hide_handle = enabled;

    if (!enabled) {
        /*
         * Disabling the handle must never strand the Dock off-screen.
         * Stop a pending hide animation and return the Dock to view.
         */
        if (dock->hide_animation_tick_id) {
            g_source_remove(dock->hide_animation_tick_id);
            dock->hide_animation_tick_id = 0;
        }

        dock->hide_animating = FALSE;
        dock->hidden = FALSE;
    }

    if (dock->hide_handle_button) {
        if (enabled) {
            gtk_widget_show(dock->hide_handle_button);
            if (dock->hide_handle_image)
                gtk_widget_show(dock->hide_handle_image);
        } else {
            gtk_widget_hide(dock->hide_handle_button);
        }
    }

    dock_compact_items(dock);
    dock_position_window(dock);
}

gboolean
dock_get_show_hide_handle(
    const Dock *dock)
{
    g_return_val_if_fail(dock != NULL, TRUE);

    return dock->show_hide_handle;
}

void
dock_set_shortcut_only(
    Dock *dock,
    gboolean enabled)
{
    g_return_if_fail(dock != NULL);

    dock->shortcut_only = !!enabled;

    if (dock->x11)
        dock_x11_set_shortcut_only(
            dock->x11,
            dock->shortcut_only);
}

gboolean
dock_get_shortcut_only(
    const Dock *dock)
{
    g_return_val_if_fail(dock != NULL, FALSE);

    return dock->shortcut_only;
}

void
dock_set_minimized_window_icons(
    Dock *dock,
    gboolean enabled)
{
    g_return_if_fail(dock != NULL);

    dock->minimized_window_icons = !!enabled;

    if (dock->x11)
        dock_x11_set_minimized_window_icons(
            dock->x11,
            dock->minimized_window_icons);
}

gboolean
dock_get_minimized_window_icons(
    const Dock *dock)
{
    g_return_val_if_fail(dock != NULL, FALSE);

    return dock->minimized_window_icons;
}

void
dock_set_minimized_title_labels(
    Dock *dock,
    gboolean enabled)
{
    g_return_if_fail(dock != NULL);

    dock->minimized_title_labels = !!enabled;

    if (dock->x11)
        dock_x11_set_minimized_title_labels(
            dock->x11,
            dock->minimized_title_labels);
}

gboolean
dock_get_minimized_title_labels(
    const Dock *dock)
{
    g_return_val_if_fail(dock != NULL, FALSE);

    return dock->minimized_title_labels;
}

void
dock_set_minimized_group_drawer(
    Dock *dock,
    gboolean enabled)
{
    g_return_if_fail(dock != NULL);

    dock->minimized_group_drawer = !!enabled;

    if (dock->x11)
        dock_x11_set_minimized_group_drawer(
            dock->x11,
            dock->minimized_group_drawer);
}

gboolean
dock_get_minimized_group_drawer(
    const Dock *dock)
{
    g_return_val_if_fail(dock != NULL, FALSE);

    return dock->minimized_group_drawer;
}

void
dock_set_minimized_all_workspaces(
    Dock *dock,
    gboolean enabled)
{
    g_return_if_fail(dock != NULL);

    dock->minimized_all_workspaces = !!enabled;

    if (dock->x11)
        dock_x11_set_minimized_all_workspaces(
            dock->x11,
            dock->minimized_all_workspaces);
}

gboolean
dock_get_minimized_all_workspaces(
    const Dock *dock)
{
    g_return_val_if_fail(dock != NULL, FALSE);

    return dock->minimized_all_workspaces;
}

void
dock_set_icon_size(
    Dock *dock,
    gint icon_size)
{
    g_return_if_fail(dock != NULL);

    icon_size = CLAMP(
        icon_size,
        16,
        64);

    if (dock->icon_size == icon_size)
        return;

    dock->icon_size = icon_size;

    /*
     * Leave enough room for GTK's native launcher button borders and padding
     * at smaller icon sizes while preserving the established large-size
     * proportions.
     */
    dock_tile_size =
        MAX(
            dock->icon_size + 16,
            (dock->icon_size * 64 + 24) / 48);

    if (dock->items) {
        for (guint i = 0;
             i < dock->items->len;
             i++) {
            DockItem *item =
                g_ptr_array_index(
                    dock->items,
                    i);

            if (!item || !item->button)
                continue;

            gtk_widget_set_size_request(
                item->button,
                DOCK_TILE_SIZE,
                DOCK_TILE_SIZE);

            if (item->is_drawer) {
                GtkWidget *image =
                    g_object_get_data(
                        G_OBJECT(item->button),
                        "dock-icon-image");

                if (image && GTK_IS_IMAGE(image))
                    gtk_image_set_pixel_size(
                        GTK_IMAGE(image),
                        dock->icon_size);
            } else {
                dock_icon_button_set_icon_size(
                    item->button,
                    dock->icon_size);
            }

            if (item->drawer &&
                item->drawer->entries) {
                for (guint j = 0;
                     j < item->drawer->entries->len;
                     j++) {
                    DockDrawerEntry *entry =
                        g_ptr_array_index(
                            item->drawer->entries,
                            j);

                    if (!entry || !entry->button)
                        continue;

                    gtk_widget_set_size_request(
                        entry->button,
                        DOCK_TILE_SIZE,
                        DOCK_TILE_SIZE);

                    dock_icon_button_set_icon_size(
                        entry->button,
                        dock->icon_size);
                }
            }
        }
    }

    if (dock->drag_proxy) {
        GtkWidget *button =
            gtk_bin_get_child(
                GTK_BIN(dock->drag_proxy));

        if (button) {
            gtk_widget_set_size_request(
                button,
                DOCK_TILE_SIZE,
                DOCK_TILE_SIZE);
            dock_icon_button_set_icon_size(
                button,
                dock->icon_size);
        }
    }

    if (dock->x11)
        dock_x11_set_icon_size(
            dock->x11,
            dock->icon_size);

    if (dock->items) {
        for (guint i = 0;
             i < dock->items->len;
             i++) {
            DockItem *item =
                g_ptr_array_index(
                    dock->items,
                    i);

            if (!item ||
                !item->drawer ||
                !item->drawer->drag_proxy)
                continue;

            GtkWidget *button =
                gtk_bin_get_child(
                    GTK_BIN(item->drawer->drag_proxy));

            if (button) {
                gtk_widget_set_size_request(
                    button,
                    DOCK_TILE_SIZE,
                    DOCK_TILE_SIZE);
                dock_icon_button_set_icon_size(
                    button,
                    dock->icon_size);
            }
        }
    }

    /*
     * Changing the tile size also changes every cached slot position.
     * Recompute those positions now instead of leaving buttons at their
     * previous geometry while the container is resized.
     */
    dock_compact_items(dock);

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(dock->box);

    if (toplevel &&
        GTK_IS_WINDOW(toplevel)) {
        gtk_window_resize(
            GTK_WINDOW(toplevel),
            dock_get_window_width(dock),
            dock_get_window_height(dock));
        gtk_widget_queue_resize(toplevel);
    }

    /*
     * Open Drawer popups use the same tile geometry. Update their child
     * positions and window size without closing the popup so live preview
     * remains smooth.
     */
    for (guint i = 0;
         i < dock->items->len;
         i++) {
        DockItem *item =
            g_ptr_array_index(
                dock->items,
                i);

        if (!item ||
            !item->drawer ||
            !item->drawer->popup)
            continue;

        DockDrawer *drawer = item->drawer;

        guint tile_count =
            MAX((guint)1, drawer->entries->len);

        gtk_window_resize(
            GTK_WINDOW(drawer->popup),
            (gint)tile_count * DOCK_TILE_SIZE,
            DOCK_TILE_SIZE);

        GtkWidget *fixed =
            gtk_bin_get_child(
                GTK_BIN(drawer->popup));

        if (!fixed || !GTK_IS_FIXED(fixed))
            continue;

        GList *children =
            gtk_container_get_children(
                GTK_CONTAINER(fixed));

        guint index = 0;

        for (GList *iter = children;
             iter;
             iter = iter->next) {
            GtkWidget *child =
                GTK_WIDGET(iter->data);

            gtk_widget_set_size_request(
                child,
                DOCK_TILE_SIZE,
                DOCK_TILE_SIZE);

            gtk_fixed_move(
                GTK_FIXED(fixed),
                child,
                (gint)(index * DOCK_TILE_SIZE),
                0);

            if (index < drawer->entries->len) {
                DockDrawerEntry *entry =
                    g_ptr_array_index(
                        drawer->entries,
                        index);

                entry->visual_x =
                    index * DOCK_TILE_SIZE;
                entry->target_x =
                    entry->visual_x;
                entry->applied_x =
                    -1;
            }

            index++;
        }

        g_list_free(children);

        dock_drawer_position_popup(
            drawer,
            drawer->animation_progress);
    }

    dock_position_window(dock);
}

gint
dock_get_icon_size(
    const Dock *dock)
{
    g_return_val_if_fail(
        dock != NULL,
        48);

    return dock->icon_size;
}

void
dock_set_opacity(
    Dock *dock,
    gint opacity_percent)
{
    g_return_if_fail(dock != NULL);

    opacity_percent =
        CLAMP(opacity_percent, 50, 100);

    dock->opacity_percent =
        opacity_percent;

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(dock->box);

    if (toplevel &&
        GTK_IS_WINDOW(toplevel))
        gtk_widget_set_opacity(
            toplevel,
            dock->opacity_percent / 100.0);

    if (dock->x11)
        dock_x11_set_opacity(
            dock->x11,
            dock->opacity_percent);

    if (dock->items) {
        for (guint i = 0;
             i < dock->items->len;
             i++) {
            DockItem *item =
                g_ptr_array_index(
                    dock->items,
                    i);

            if (!item ||
                !item->drawer ||
                !item->drawer->popup)
                continue;

            gtk_widget_set_opacity(
                item->drawer->popup,
                dock->opacity_percent / 100.0);
        }
    }
}

gint
dock_get_opacity(
    const Dock *dock)
{
    g_return_val_if_fail(
        dock != NULL,
        100);

    return dock->opacity_percent;
}

void
dock_set_bitmap_background(
    Dock *dock,
    gboolean enabled,
    const gchar *image_path,
    DockBitmapBackgroundMode mode)
{
    g_return_if_fail(dock != NULL);

    gchar *stored_path =
        image_path && *image_path ?
        g_strdup(image_path) :
        NULL;
    enabled = !!enabled && stored_path != NULL;

    GdkScreen *screen = gdk_screen_get_default();

    if (dock->bitmap_background_provider) {
        if (screen) {
            gtk_style_context_remove_provider_for_screen(
                screen,
                GTK_STYLE_PROVIDER(dock->bitmap_background_provider));
        }
        g_clear_object(&dock->bitmap_background_provider);
    }

    dock->bitmap_background_enabled = enabled;
    dock->bitmap_background_mode =
        mode == DOCK_BITMAP_BACKGROUND_EXTEND ?
            DOCK_BITMAP_BACKGROUND_EXTEND :
            DOCK_BITMAP_BACKGROUND_REPEAT;
    g_free(dock->bitmap_background_path);
    dock->bitmap_background_path = stored_path;

    if (!enabled || !screen ||
        !g_file_test(dock->bitmap_background_path, G_FILE_TEST_IS_REGULAR))
        return;

    GError *image_error = NULL;
    GdkPixbuf *test_image =
        gdk_pixbuf_new_from_file(dock->bitmap_background_path, &image_error);
    if (!test_image) {
        g_warning(
            "Unable to load Crepido bitmap background '%s': %s",
            dock->bitmap_background_path,
            image_error ? image_error->message : "unsupported image");
        g_clear_error(&image_error);
        return;
    }
    g_object_unref(test_image);

    GError *uri_error = NULL;
    gchar *image_uri =
        g_filename_to_uri(dock->bitmap_background_path, NULL, &uri_error);
    if (!image_uri) {
        g_warning(
            "Unable to create a URI for Crepido bitmap background: %s",
            uri_error ? uri_error->message : "unknown error");
        g_clear_error(&uri_error);
        return;
    }

    const gchar *background_repeat =
        dock->bitmap_background_mode == DOCK_BITMAP_BACKGROUND_EXTEND ?
            "no-repeat" : "repeat";
    const gchar *background_size =
        dock->bitmap_background_mode == DOCK_BITMAP_BACKGROUND_EXTEND ?
            "100% 100%" : "auto";

    gchar *css =
        g_strdup_printf(
            "button.crepido-bitmap-block { "
            "background-image: url(\"%s\"); "
            "background-repeat: %s; "
            "background-position: left top; "
            "background-size: %s; "
            "}",
            image_uri,
            background_repeat,
            background_size);

    GtkCssProvider *provider = gtk_css_provider_new();
    GError *css_error = NULL;
    gtk_css_provider_load_from_data(provider, css, -1, &css_error);
    g_free(css);
    g_free(image_uri);

    if (css_error) {
        g_warning(
            "Unable to apply Crepido bitmap background: %s",
            css_error->message);
        g_clear_error(&css_error);
        g_object_unref(provider);
        return;
    }

    gtk_style_context_add_provider_for_screen(
        screen,
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    dock->bitmap_background_provider = provider;
}

void
dock_set_monitor_index(
    Dock *dock,
    gint monitor_index)
{
    g_return_if_fail(dock != NULL);

    GdkDisplay *display =
        dock->box ?
        gtk_widget_get_display(dock->box) :
        NULL;

    if (display) {
        gint monitor_count =
            gdk_display_get_n_monitors(
                display);

        if (monitor_count > 0) {
            if (monitor_index < 0 ||
                monitor_index >= monitor_count) {
                GdkMonitor *primary =
                    gdk_display_get_primary_monitor(
                        display);

                monitor_index = 0;

                for (gint i = 0;
                     i < monitor_count;
                     i++) {
                    if (gdk_display_get_monitor(
                            display,
                            i) == primary) {
                        monitor_index = i;
                        break;
                    }
                }
            }
        } else {
            monitor_index = -1;
        }
    } else {
        monitor_index = -1;
    }

    GPtrArray *open_drawers =
        g_ptr_array_new();

    if (dock->items) {
        for (guint i = 0;
             i < dock->items->len;
             i++) {
            DockItem *item =
                g_ptr_array_index(
                    dock->items,
                    i);

            if (!item->is_drawer ||
                !item->drawer ||
                !item->drawer->popup)
                continue;

            g_ptr_array_add(
                open_drawers,
                item->drawer);

            dock_drawer_destroy_popup(
                item->drawer);
        }
    }

    dock->monitor_index = monitor_index;

    GdkMonitor *selected_monitor =
        display && monitor_index >= 0 &&
        monitor_index < gdk_display_get_n_monitors(display) ?
        gdk_display_get_monitor(display, monitor_index) :
        NULL;
    dock_monitor_cache_selected(dock, selected_monitor);

    dock_position_window(dock);

    for (guint i = 0;
         i < open_drawers->len;
         i++) {
        DockDrawer *drawer =
            g_ptr_array_index(
                open_drawers,
                i);

        dock_drawer_open(drawer);
    }

    g_ptr_array_free(
        open_drawers,
        TRUE);
}

gint
dock_get_monitor_index(
    const Dock *dock)
{
    g_return_val_if_fail(
        dock != NULL,
        -1);

    return dock->monitor_index;
}

gboolean
dock_get_right_side(
    const Dock *dock)
{
    g_return_val_if_fail(
        dock != NULL,
        TRUE);

    return dock->on_right_side;
}

void
dock_set_position_mode(
    Dock *dock,
    DockPositionMode mode)
{
    g_return_if_fail(dock != NULL);

    if (mode < DOCK_POSITION_NORMAL ||
        mode > DOCK_POSITION_KEEP_ON_TOP)
        mode = DOCK_POSITION_NORMAL;

    dock->position_mode = mode;

    GtkWidget *toplevel =
        gtk_widget_get_toplevel(
            dock->box);

    if (!toplevel ||
        !GTK_IS_WINDOW(toplevel))
        return;

    /* The reveal button shares the Dock's top-level window. */
    GtkWidget *windows[] = {
        toplevel
    };

    for (guint i = 0; i < G_N_ELEMENTS(windows); i++) {
        GtkWidget *window = windows[i];

        if (!window || !GTK_IS_WINDOW(window))
            continue;

        switch (mode) {
        case DOCK_POSITION_KEEP_ON_TOP:
            gtk_window_set_keep_below(
                GTK_WINDOW(window),
                FALSE);
            gtk_window_set_keep_above(
                GTK_WINDOW(window),
                TRUE);
            break;

        case DOCK_POSITION_AUTO_RAISE_LOWER:
            gtk_window_set_keep_above(
                GTK_WINDOW(window),
                FALSE);
            gtk_window_set_keep_below(
                GTK_WINDOW(window),
                FALSE);
            break;

        case DOCK_POSITION_NORMAL:
        default:
            gtk_window_set_keep_above(
                GTK_WINDOW(window),
                FALSE);
            gtk_window_set_keep_below(
                GTK_WINDOW(window),
                TRUE);
            break;
        }
    }

    switch (mode) {
    case DOCK_POSITION_KEEP_ON_TOP:
        dock_raise_window(dock);
        break;

    case DOCK_POSITION_AUTO_RAISE_LOWER:
    case DOCK_POSITION_NORMAL:
    default:
        dock_lower_window(dock);
        break;
    }
}

DockPositionMode
dock_get_position_mode(
    const Dock *dock)
{
    g_return_val_if_fail(
        dock != NULL,
        DOCK_POSITION_NORMAL);

    return dock->position_mode;
}

GtkWidget *
dock_get_widget(
    Dock *dock)
{
    g_return_val_if_fail(
        dock != NULL,
        NULL);

    return dock->box;
}

gboolean
dock_is_dragging(
    const Dock *dock)
{
    g_return_val_if_fail(
        dock != NULL,
        FALSE);

    return dock->dragging;
}
