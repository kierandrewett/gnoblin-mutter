/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Mutter utilities */

/*
 * Copyright (C) 2001 Havoc Pennington
 * Copyright (C) 2005 Elijah Newren
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <glib.h>
#include <glib-object.h>

#include "meta/common.h"
#include "meta/meta-debug.h"
#include "meta/meta-later.h"

META_EXPORT
gboolean meta_is_verbose  (void);

META_EXPORT
void meta_bug        (const char *format,
                      ...) G_GNUC_PRINTF (1, 2);

META_EXPORT
void meta_fatal      (const char *format,
                      ...) G_GNUC_PRINTF (1, 2) G_GNUC_NORETURN G_ANALYZER_NORETURN;

/**
 * MetaDebugPaintFlag:
 * @META_DEBUG_PAINT_NONE: default
 * @META_DEBUG_PAINT_OPAQUE_REGION: paint opaque regions
 * @META_DEBUG_PAINT_SYNC_CURSOR_PRIMARY: make cursor updates await compositing
 *   frames
 * @META_DEBUG_PAINT_DISABLE_DIRECT_SCANOUT: always composite frames
 */
typedef enum
{
  META_DEBUG_PAINT_NONE          = 0,
  META_DEBUG_PAINT_OPAQUE_REGION = 1 << 0,
  META_DEBUG_PAINT_SYNC_CURSOR_PRIMARY = 1 << 1,
  META_DEBUG_PAINT_DISABLE_DIRECT_SCANOUT = 1 << 2,
  META_DEBUG_PAINT_IGNORE_COLOR_STATE_FOR_DIRECT_SCANOUT = 1 << 3,
} MetaDebugPaintFlag;

META_EXPORT
void meta_add_verbose_topic    (MetaDebugTopic topic);

META_EXPORT
void meta_remove_verbose_topic (MetaDebugTopic topic);

META_EXPORT
gint  meta_unsigned_long_equal (gconstpointer v1,
                                gconstpointer v2);

META_EXPORT
guint meta_unsigned_long_hash  (gconstpointer v);

META_EXPORT
const char* meta_frame_type_to_string (MetaFrameType type);
META_EXPORT
const char* meta_gravity_to_string (MetaGravity gravity);

META_EXPORT
char* meta_external_binding_name_for_action (guint keybinding_action);

META_EXPORT
char* meta_g_utf8_strndup (const gchar *src, gsize n);

META_EXPORT
void meta_read_bytes (int                 fd,
                      uint32_t            offset,
                      uint32_t            length,
                      GAsyncReadyCallback callback,
                      gpointer            user_data);

META_EXPORT
gboolean meta_read_bytes_finish (GAsyncResult  *result,
                                 uint8_t      **bytes,
                                 uint32_t      *length,
                                 GError       **error);

META_EXPORT
void meta_add_debug_paint_flag (MetaDebugPaintFlag flag);

META_EXPORT
void meta_remove_debug_paint_flag (MetaDebugPaintFlag flag);

META_EXPORT
MetaDebugPaintFlag meta_get_debug_paint_flags (void);

META_EXPORT
char * meta_accelerator_name (ClutterModifierType accelerator_mods,
                              unsigned int        accelerator_key);

#include "meta/types.h"

META_EXPORT
int meta_gnoblin_layer_anchor (MetaWindow *window);

META_EXPORT
const char *meta_gnoblin_layer_namespace (MetaWindow *window);

META_EXPORT
gboolean meta_gnoblin_window_pattern_match (const char *pattern,
                                            const char *value,
                                            GError **error);

META_EXPORT
const char *meta_gnoblin_workspace_id (MetaWorkspace *workspace);

META_EXPORT
gboolean meta_gnoblin_window_frame_set (MetaWindow *window, GVariant *policy, GError **error);
META_EXPORT
GVariant *meta_gnoblin_window_frame_get (MetaWindow *window);
META_EXPORT
void meta_gnoblin_window_frame_style (MetaWindow *window, GVariant *options);
META_EXPORT
gboolean meta_gnoblin_frame_renderers_configure (GVariant *services, gboolean restart, GError **error);

META_EXPORT
GVariant *meta_gnoblin_dispatch_native_api (MetaDisplay *display,
                                            const char  *method,
                                            GVariant    *arguments,
                                            GError     **error);

META_EXPORT
gboolean meta_gnoblin_initialize_native_workspaces (MetaDisplay *display,
                                                     GError     **error);

META_EXPORT
void meta_display_dispatch_gnoblin_native_event (MetaDisplay *display,
                                                 const char  *event,
                                                 GVariant    *payload);
