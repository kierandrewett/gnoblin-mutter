/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2001 Havoc Pennington
 * Copyright (C) 2002, 2003, 2004 Red Hat, Inc.
 * Copyright (C) 2003, 2004 Rob Adams
 * Copyright (C) 2004-2006 Elijah Newren
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

#include "meta/display.h"

/**
 * META_PRIORITY_EVENTS: (value 0)
 *
 * This is the priority that events from the X server are given in the main loop.
 */
#define META_PRIORITY_EVENTS    (G_PRIORITY_DEFAULT)


void meta_display_init_events (MetaDisplay *display);
void meta_display_free_events (MetaDisplay *display);

/* Native shortcut capture runs before normal-session keyboard consumers.
 * While active, every key press and release is delivered to the registered
 * handler and consumed. Other event types continue through normal handling. */
typedef void (* MetaDisplayNativeKeyCaptureFunc) (const ClutterEvent *event,
                                                  gpointer           user_data);

gboolean meta_display_register_native_key_capture_handler (MetaDisplay                     *display,
                                                            MetaDisplayNativeKeyCaptureFunc  callback,
                                                            gpointer                         user_data,
                                                            GDestroyNotify                   destroy);
void meta_display_unregister_native_key_capture_handler (MetaDisplay *display,
                                                         gpointer     user_data);
gboolean meta_display_start_native_key_capture (MetaDisplay *display);
void meta_display_stop_native_key_capture (MetaDisplay *display);
