/*
 * crepido
 *
 * Small Window Maker-inspired workspace Clip for MATE/Marco on X11.
 * Workspace state is read from and switched through EWMH, so the Clip
 * remains synchronized with MATE's existing Workspace Switcher.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "dock.h"

G_BEGIN_DECLS

typedef struct _DockClip DockClip;

DockClip *dock_clip_new(Dock *dock);
void dock_clip_free(DockClip *clip);

G_END_DECLS
