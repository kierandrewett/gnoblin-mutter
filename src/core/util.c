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


#define _POSIX_C_SOURCE 200112L /* for fdopen() */

#include "config.h"

#include "core/display-private.h"
#include "core/util-private.h"
#include "core/gnoblin-native-control.h"

#include <gio/gunixinputstream.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#ifdef HAVE_SYS_PRCTL
#include <sys/prctl.h>
#endif

#include "clutter/clutter-mutter.h"
#include "cogl/cogl.h"
#include "meta/common.h"
#include "meta/main.h"
#include "meta/meta-backend.h"
#include "meta/meta-context.h"
#include "meta/window.h"
#include "meta/workspace.h"

static const GDebugKey meta_debug_keys[] = {
  { "focus", META_DEBUG_FOCUS },
  { "workarea", META_DEBUG_WORKAREA },
  { "stack", META_DEBUG_STACK },
  { "sm", META_DEBUG_SM },
  { "events", META_DEBUG_EVENTS },
  { "window-state", META_DEBUG_WINDOW_STATE },
  { "window-ops", META_DEBUG_WINDOW_OPS },
  { "geometry", META_DEBUG_GEOMETRY },
  { "placement", META_DEBUG_PLACEMENT },
  { "display", META_DEBUG_DISPLAY },
  { "keybindings", META_DEBUG_KEYBINDINGS },
  { "sync", META_DEBUG_SYNC },
  { "startup", META_DEBUG_STARTUP },
  { "prefs", META_DEBUG_PREFS },
  { "edge-resistance", META_DEBUG_EDGE_RESISTANCE },
  { "dbus", META_DEBUG_DBUS },
  { "input", META_DEBUG_INPUT },
  { "wayland", META_DEBUG_WAYLAND },
  { "kms", META_DEBUG_KMS },
  { "screen-cast", META_DEBUG_SCREEN_CAST },
  { "remote-desktop", META_DEBUG_REMOTE_DESKTOP },
  { "backend", META_DEBUG_BACKEND },
  { "render", META_DEBUG_RENDER },
  { "color", META_DEBUG_COLOR },
  { "input-events", META_DEBUG_INPUT_EVENTS },
  { "eis", META_DEBUG_EIS },
  { "kms-deadline", META_DEBUG_KMS_DEADLINE },
  { "session-management", META_DEBUG_SESSION_MANAGEMENT },
  { "x11", META_DEBUG_X11 },
  { "workspaces", META_DEBUG_WORKSPACES },
};

static const GDebugKey meta_paint_debug_keys[] = {
  { "opaque-region", META_DEBUG_PAINT_OPAQUE_REGION },
  { "sync-cursor-primary", META_DEBUG_PAINT_SYNC_CURSOR_PRIMARY },
  { "disable-direct-scanout", META_DEBUG_PAINT_DISABLE_DIRECT_SCANOUT },
  { "ignore-color-state-for-direct-scanout", META_DEBUG_PAINT_IGNORE_COLOR_STATE_FOR_DIRECT_SCANOUT },
};

typedef struct _MetaReadBytesContext
{
  int fd;
  uint32_t offset;
  uint32_t length;
  uint8_t *bytes;
} MetaReadBytesContext;

static gint verbose_topics = 0;
static int debug_paint_flags = 0;
static GLogLevelFlags mutter_log_level = G_LOG_LEVEL_MESSAGE;

#ifdef WITH_VERBOSE_MODE
static FILE* logfile = NULL;

static void
ensure_logfile (void)
{
  if (logfile == NULL && g_getenv ("MUTTER_USE_LOGFILE"))
    {
      char *filename = NULL;
      char *tmpl;
      int fd;
      g_autoptr (GError) error = NULL;

      tmpl = g_strdup_printf ("mutter-%d-debug-log-XXXXXX",
                              (int) getpid ());

      fd = g_file_open_tmp (tmpl,
                            &filename,
                            &error);

      g_free (tmpl);

      if (error != NULL)
        {
          g_warning ("Failed to open debug log: %s",
                     error->message);
          return;
        }

      logfile = fdopen (fd, "w");

      if (logfile == NULL)
        {
          g_warning ("Failed to fdopen() log file %s: %s",
                     filename, strerror (errno));
          close (fd);
        }
      else
        {
          g_printerr ("Opened log file %s", filename);
        }

      g_free (filename);
    }
}

#endif

/**
 * meta_gnoblin_layer_anchor:
 * @window: window to inspect
 *
 * Returns: layer-shell anchor flags, or -1 for other windows
 */
int
meta_gnoblin_layer_anchor (MetaWindow *window)
{
  return GPOINTER_TO_INT (g_object_get_data (G_OBJECT (window),
                                             "gnoblin-layer-anchor")) - 1;
}

/**
 * meta_gnoblin_layer_namespace:
 * @window: window to inspect
 *
 * Returns: (transfer none) (nullable): the layer namespace, or NULL
 */
const char *
meta_gnoblin_layer_namespace (MetaWindow *window)
{
  return g_object_get_data (G_OBJECT (window), "gnoblin-layer-namespace");
}

gboolean
meta_is_gnoblin_session (void)
{
  return gnoblin_native_control_is_session (NULL);
}

gboolean
meta_is_verbose (void)
{
  return verbose_topics != 0;
}

void
meta_set_verbose (gboolean setting)
{
#ifndef WITH_VERBOSE_MODE
  if (setting)
    meta_fatal (_("Mutter was compiled without support for verbose mode"));
#endif

  if (setting)
    meta_add_verbose_topic (META_DEBUG_VERBOSE);
  else
    meta_remove_verbose_topic (META_DEBUG_VERBOSE);
}

/**
 * meta_add_verbose_topic:
 * @topic: Topic for which logging will be started
 *
 * Ensure log messages for the given topic @topic
 * will be printed.
 */
void
meta_add_verbose_topic (MetaDebugTopic topic)
{
  if (verbose_topics == META_DEBUG_VERBOSE)
    return;

#ifdef WITH_VERBOSE_MODE
  ensure_logfile ();
#endif

  if (topic == META_DEBUG_VERBOSE)
    verbose_topics = META_DEBUG_VERBOSE;
  else
    verbose_topics |= topic;
}

/**
 * meta_remove_verbose_topic:
 * @topic: Topic for which logging will be stopped
 *
 * Stop printing log messages for the given topic @topic.
 *
 * Note that this method does not stack with [func@Meta.add_verbose_topic];
 * i.e. if two calls to [func@Meta.add_verbose_topic] for the same
 * topic are made, one call to [func@Meta.remove_verbose_topic]  will
 * remove it.
 */
void
meta_remove_verbose_topic (MetaDebugTopic topic)
{
  if (topic == META_DEBUG_VERBOSE)
    verbose_topics = 0;
  else
    verbose_topics &= ~topic;
}

void
meta_init_debug_utils (void)
{
  const char *debug_env;

#ifdef HAVE_SYS_PRCTL
  prctl (PR_SET_DUMPABLE, 1);
#endif

  if (g_getenv ("MUTTER_VERBOSE"))
    meta_set_verbose (TRUE);

  debug_env = g_getenv ("MUTTER_DEBUG");
  if (debug_env)
    {
      MetaDebugTopic topics;

      topics = g_parse_debug_string (debug_env,
                                     meta_debug_keys,
                                     G_N_ELEMENTS (meta_debug_keys));
      meta_add_verbose_topic (topics);
    }

  debug_env = g_getenv ("MUTTER_DEBUG_PAINT");
  if (debug_env)
    {
      MetaDebugPaintFlag flags;

      flags = g_parse_debug_string (debug_env,
                                    meta_paint_debug_keys,
                                    G_N_ELEMENTS (meta_paint_debug_keys));
      meta_add_debug_paint_flag (flags);
    }

  if (g_test_initialized ())
    mutter_log_level = G_LOG_LEVEL_DEBUG;
}

char *
meta_g_utf8_strndup (const gchar *src,
                     gsize        n)
{
  const gchar *s = src;
  while (n && *s)
    {
      s = g_utf8_next_char (s);
      n--;
    }

  return g_strndup (src, s - src);
}

static void
meta_read_bytes_context_free (MetaReadBytesContext *context)
{
  g_clear_fd (&context->fd, NULL);
  g_clear_pointer (&context->bytes, g_free);
  g_free (context);
}

static void
meta_read_bytes_in_thread (GTask        *task,
                           gpointer      source_object,
                           gpointer      task_data,
                           GCancellable *cancellable)
{
  MetaReadBytesContext *context = task_data;
  g_autoptr (GInputStream) input_stream = NULL;
  g_autofree uint8_t *bytes = NULL;
  g_autoptr (GError) error = NULL;
  int skipped;

  input_stream = G_INPUT_STREAM (g_unix_input_stream_new (context->fd, FALSE));

  skipped = g_input_stream_skip (input_stream,
                                 context->offset,
                                 NULL,
                                 &error);
  if (skipped < 0)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  bytes = g_malloc (context->length);
  if (!g_input_stream_read_all (input_stream,
                                bytes,
                                context->length,
                                NULL,
                                NULL,
                                &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  context->bytes = g_steal_pointer (&bytes);

  g_task_return_boolean (task, TRUE);
}

void
meta_read_bytes (int                 fd,
                 uint32_t            offset,
                 uint32_t            length,
                 GAsyncReadyCallback callback,
                 gpointer            user_data)
{
  g_autoptr (GTask) task = NULL;
  MetaReadBytesContext *context;

  task = g_task_new (NULL, NULL, callback, user_data);

  context = g_new0 (MetaReadBytesContext, 1);
  context->fd = dup (fd);
  context->offset = offset;
  context->length = length;

  g_task_set_task_data (task, context,
                        (GDestroyNotify) meta_read_bytes_context_free);
  g_task_run_in_thread (task, meta_read_bytes_in_thread);
}

gboolean
meta_read_bytes_finish (GAsyncResult  *result,
                        uint8_t      **bytes,
                        uint32_t      *length,
                        GError       **error)
{
  MetaReadBytesContext *context = g_task_get_task_data (G_TASK (result));

  *bytes = g_steal_pointer (&context->bytes);

  if (length)
    *length = context->length;

  return g_task_propagate_boolean (G_TASK (result), error);
}

static int
utf8_fputs (const char *str,
            FILE       *f)
{
  char *l;
  int retval;

  l = g_locale_from_utf8 (str, -1, NULL, NULL, NULL);

  if (l == NULL)
    retval = fputs (str, f); /* just print it anyway, better than nothing */
  else
    retval = fputs (l, f);

  g_free (l);

  return retval;
}

#ifdef WITH_VERBOSE_MODE
const char *
meta_topic_to_string (MetaDebugTopic topic)
{
  switch (topic)
    {
    case META_DEBUG_FOCUS:
      return "FOCUS";
    case META_DEBUG_WORKAREA:
      return "WORKAREA";
    case META_DEBUG_STACK:
      return "STACK";
    case META_DEBUG_SM:
      return "SM";
    case META_DEBUG_EVENTS:
      return "EVENTS";
    case META_DEBUG_WINDOW_STATE:
      return "WINDOW_STATE";
    case META_DEBUG_WINDOW_OPS:
      return "WINDOW_OPS";
    case META_DEBUG_PLACEMENT:
      return "PLACEMENT";
    case META_DEBUG_DISPLAY:
      return "DISPLAY";
    case META_DEBUG_GEOMETRY:
      return "GEOMETRY";
    case META_DEBUG_KEYBINDINGS:
      return "KEYBINDINGS";
    case META_DEBUG_SYNC:
      return "SYNC";
    case META_DEBUG_STARTUP:
      return "STARTUP";
    case META_DEBUG_PREFS:
      return "PREFS";
    case META_DEBUG_EDGE_RESISTANCE:
      return "EDGE_RESISTANCE";
    case META_DEBUG_DBUS:
      return "DBUS";
    case META_DEBUG_INPUT:
      return "INPUT";
    case META_DEBUG_WAYLAND:
      return "WAYLAND";
    case META_DEBUG_KMS:
      return "KMS";
    case META_DEBUG_SCREEN_CAST:
      return "SCREEN_CAST";
    case META_DEBUG_REMOTE_DESKTOP:
      return "REMOTE_DESKTOP";
    case META_DEBUG_BACKEND:
      return "BACKEND";
    case META_DEBUG_RENDER:
      return "RENDER";
    case META_DEBUG_COLOR:
      return "COLOR";
    case META_DEBUG_VERBOSE:
      return "VERBOSE";
    case META_DEBUG_INPUT_EVENTS:
      return "INPUT_EVENTS";
    case META_DEBUG_EIS:
      return "EIS";
    case META_DEBUG_KMS_DEADLINE:
      return "KMS_DEADLINE";
    case META_DEBUG_SESSION_MANAGEMENT:
      return "SESSION_MANAGEMENT";
    case META_DEBUG_X11:
      return "X11";
    case META_DEBUG_WORKSPACES:
      return "WORKSPACES";
    }

  return "WM";
}

gboolean
meta_is_topic_enabled (MetaDebugTopic topic)
{
  if (verbose_topics == 0)
    return FALSE;

  if (topic == META_DEBUG_VERBOSE && verbose_topics != META_DEBUG_VERBOSE)
    return FALSE;

  return !!(verbose_topics & topic);
}

#else /* WITH_VERBOSE_MODE */

gboolean
meta_is_topic_enabled (MetaDebugTopic topic)
{
  return FALSE;
}
#endif /* WITH_VERBOSE_MODE */

void
meta_bug (const char *format, ...)
{
  va_list args;
  gchar *str;
  FILE *out;

  g_return_if_fail (format != NULL);

  va_start (args, format);
  str = g_strdup_vprintf (format, args);
  va_end (args);

#ifdef WITH_VERBOSE_MODE
  out = logfile ? logfile : stderr;
#else
  out = stderr;
#endif

  utf8_fputs ("Bug in window manager: ", out);
  utf8_fputs (str, out);
  utf8_fputs ("\n", out);

  fflush (out);

  g_free (str);

  /* stop us in a debugger */
  abort ();
}

void
meta_fatal (const char *format, ...)
{
  va_list args;
  gchar *str;
  FILE *out;

  g_warn_if_fail (format);
  if (!format)
    meta_exit (META_EXIT_ERROR);

  va_start (args, format);
  str = g_strdup_vprintf (format, args);
  va_end (args);

#ifdef WITH_VERBOSE_MODE
  out = logfile ? logfile : stderr;
#else
  out = stderr;
#endif

  utf8_fputs ("Window manager error: ", out);
  utf8_fputs (str, out);
  utf8_fputs ("\n", out);

  fflush (out);

  g_free (str);

  meta_exit (META_EXIT_ERROR);
}

void
meta_exit (MetaExitCode code)
{

  exit (code);
}

gint
meta_unsigned_long_equal (gconstpointer v1,
                          gconstpointer v2)
{
  return *((const gulong*) v1) == *((const gulong*) v2);
}

guint
meta_unsigned_long_hash  (gconstpointer v)
{
  gulong val = * (const gulong *) v;

  /* I'm not sure this works so well. */
#if GLIB_SIZEOF_LONG > 4
  return (guint) (val ^ (val >> 32));
#else
  return val;
#endif
}

const char*
meta_gravity_to_string (MetaGravity gravity)
{
  switch (gravity)
    {
    case META_GRAVITY_NORTH_WEST:
      return "META_GRAVITY_NORTH_WEST";
      break;
    case META_GRAVITY_NORTH:
      return "META_GRAVITY_NORTH";
      break;
    case META_GRAVITY_NORTH_EAST:
      return "META_GRAVITY_NORTH_EAST";
      break;
    case META_GRAVITY_WEST:
      return "META_GRAVITY_WEST";
      break;
    case META_GRAVITY_CENTER:
      return "META_GRAVITY_CENTER";
      break;
    case META_GRAVITY_EAST:
      return "META_GRAVITY_EAST";
      break;
    case META_GRAVITY_SOUTH_WEST:
      return "META_GRAVITY_SOUTH_WEST";
      break;
    case META_GRAVITY_SOUTH:
      return "META_GRAVITY_SOUTH";
      break;
    case META_GRAVITY_SOUTH_EAST:
      return "META_GRAVITY_SOUTH_EAST";
      break;
    case META_GRAVITY_STATIC:
      return "META_GRAVITY_STATIC";
      break;
    default:
      return "META_GRAVITY_NORTH_WEST";
      break;
    }
}

char*
meta_external_binding_name_for_action (guint keybinding_action)
{
  return g_strdup_printf ("external-grab-%u", keybinding_action);
}

char *
meta_generate_random_id (GRand *rand,
                         int    length)
{
  char *id;
  int i;

  /* Generate a random string of printable ASCII characters. */

  id = g_new0 (char, length + 1);
  for (i = 0; i < length; i++)
    id[i] = (char) g_rand_int_range (rand, 32, 127);

  return id;
}

void
meta_add_debug_paint_flag (MetaDebugPaintFlag flag)
{
  debug_paint_flags |= flag;
}

void
meta_remove_debug_paint_flag (MetaDebugPaintFlag flag)
{
  debug_paint_flags &= ~flag;
}

MetaDebugPaintFlag
meta_get_debug_paint_flags (void)
{
  return debug_paint_flags;
}

void
meta_log (const char *format, ...)
{
  va_list args;

  va_start (args, format);
  g_logv (G_LOG_DOMAIN, mutter_log_level, format, args);
  va_end (args);
}

char *
meta_encode_hex (gpointer data,
                 size_t   size)
{
  uint8_t *data_u8 = data;
  GString *encoded;
  size_t i;

  encoded = g_string_new (NULL);
  for (i = 0; i < size; i++)
    g_string_append_printf (encoded, "%02x", data_u8[i]);

  return g_string_free_and_steal (encoded);
}

gboolean
meta_display_dispatch_gnoblin_event (MetaDisplay *display,
                                     const char  *event,
                                     GVariant    *payload)
{
#ifdef HAVE_WAYLAND
  g_autoptr (GError) error = NULL;
  gboolean claimed = FALSE;

  if (!gnoblin_native_control_is_session (display))
    return FALSE;
  if (!gnoblin_native_control_dispatch_runtime_event (display, event, payload,
                                                      &claimed, &error))
    g_warning ("gnoblin runtime event %s: %s", event,
               error ? error->message : "could not queue event");
  return claimed;
#else
  return FALSE;
#endif
}

static GVariant *
gnoblin_native_window_list (MetaDisplay *display,
                            GVariant    *arguments,
                            GError     **error)
{
  MetaBackend *backend = meta_context_get_backend (meta_display_get_context (display));
  if (meta_backend_get_gnoblin_session_lock_active (backend))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "window listing is unavailable while the session is locked");
      return NULL;
    }

  const char *app_filter = NULL;
  const char *title_filter = NULL;
  gboolean focused_only = FALSE;
  g_autoptr (GVariant) app_value = NULL;
  g_autoptr (GVariant) title_value = NULL;
  g_autoptr (GVariant) focused_value = NULL;
  if (arguments)
    {
      app_value = g_variant_lookup_value (arguments, "app_id", NULL);
      title_value = g_variant_lookup_value (arguments, "title", NULL);
      focused_value = g_variant_lookup_value (arguments, "focused", NULL);
      if ((app_value && !g_variant_is_of_type (app_value, G_VARIANT_TYPE_STRING)) ||
          (title_value && !g_variant_is_of_type (title_value, G_VARIANT_TYPE_STRING)) ||
          (focused_value && !g_variant_is_of_type (focused_value, G_VARIANT_TYPE_BOOLEAN)))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                               "window filters must be strings or a focused boolean");
          return NULL;
        }
      if (app_value)
        app_filter = g_variant_get_string (app_value, NULL);
      if (title_value)
        title_filter = g_variant_get_string (title_value, NULL);
      if (focused_value)
        focused_only = g_variant_get_boolean (focused_value);
    }
  g_autofree char *folded_filter = title_filter ? g_utf8_casefold (title_filter, -1) : NULL;
  MetaWindow *focused = meta_display_get_focus_window (display);
  g_autoptr (GList) windows = meta_display_list_all_windows (display);
  GVariantBuilder entries;
  GVariantBuilder result;
  g_variant_builder_init (&entries, G_VARIANT_TYPE ("aa{sv}"));
  for (GList *item = windows; item; item = item->next)
    {
      MetaWindow *window = item->data;
      if (meta_window_is_skip_taskbar (window) ||
          meta_window_is_override_redirect (window))
        continue;

      const char *title = meta_window_get_title (window);
      const char *gtk_id = meta_window_get_gtk_application_id (window);
      const char *wm_class = meta_window_get_wm_class (window);
      const char *app_id = gtk_id && *gtk_id ? gtk_id : wm_class;
      if (app_filter && g_strcmp0 (app_filter, app_id) != 0)
        continue;
      if (focused_only && window != focused)
        continue;
      if (folded_filter)
        {
          g_autofree char *folded_title = g_utf8_casefold (title ? title : "", -1);
          if (!g_strstr_len (folded_title, -1, folded_filter))
            continue;
        }

      g_autofree char *id = g_strdup_printf ("%u", meta_window_get_stable_sequence (window));
      MetaWorkspace *workspace = meta_window_get_workspace (window);
      MtkRectangle frame;
      meta_window_get_frame_rect (window, &frame);
      GVariantBuilder geometry;
      GVariantBuilder entry;
      g_variant_builder_init (&geometry, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&geometry, "{sv}", "x", g_variant_new_int32 (frame.x));
      g_variant_builder_add (&geometry, "{sv}", "y", g_variant_new_int32 (frame.y));
      g_variant_builder_add (&geometry, "{sv}", "width", g_variant_new_int32 (frame.width));
      g_variant_builder_add (&geometry, "{sv}", "height", g_variant_new_int32 (frame.height));
      g_variant_builder_init (&entry, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&entry, "{sv}", "id", g_variant_new_string (id));
      g_variant_builder_add (&entry, "{sv}", "title", g_variant_new_string (title ? title : ""));
      g_variant_builder_add (&entry, "{sv}", "appId", g_variant_new_string (app_id ? app_id : ""));
      g_variant_builder_add (&entry, "{sv}", "gtkAppId", g_variant_new_string (gtk_id ? gtk_id : ""));
      g_variant_builder_add (&entry, "{sv}", "wmClass", g_variant_new_string (wm_class ? wm_class : ""));
      g_variant_builder_add (&entry, "{sv}", "ruleAppId", g_variant_new_string (app_id ? app_id : ""));
      g_variant_builder_add (&entry, "{sv}", "focused", g_variant_new_boolean (window == focused));
      if (workspace)
        {
          int number = meta_workspace_index (workspace) + 1;
          g_variant_builder_add (&entry, "{sv}", "workspace", g_variant_new_int32 (number));
          g_variant_builder_add (&entry, "{sv}", "workspaceNumber", g_variant_new_int32 (number));
        }
      g_variant_builder_add (&entry, "{sv}", "monitorIndex", g_variant_new_int32 (meta_window_get_monitor (window)));
      g_variant_builder_add (&entry, "{sv}", "maximized", g_variant_new_boolean (meta_window_is_maximized (window)));
      g_variant_builder_add (&entry, "{sv}", "fullscreen", g_variant_new_boolean (meta_window_is_fullscreen (window)));
      g_variant_builder_add (&entry, "{sv}", "geometry", g_variant_builder_end (&geometry));
      g_variant_builder_add_value (&entries, g_variant_builder_end (&entry));
    }
  g_variant_builder_init (&result, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&result, "{sv}", "windows", g_variant_builder_end (&entries));
  return g_variant_ref_sink (g_variant_builder_end (&result));
}


static GVariant *
gnoblin_native_window_action (MetaDisplay *display,
                              GVariant    *arguments,
                              GError     **error)
{
  static const char * const supported[] = {
    "above", "unabove", "stick", "unstick", "close", "minimize",
    "restore", "maximize", "unmaximize", "fullscreen", "unfullscreen",
    "monitor", NULL
  };
  const char *action = NULL;
  const char *selector = "active";
  MetaWindow *window = NULL;
  g_autoptr (GList) windows = NULL;
  g_autoptr (GVariant) window_value = NULL;
  g_autofree char *window_id = NULL;
  guint32 stable_sequence;
  GVariantBuilder result;

  if (!arguments || !g_variant_is_of_type (arguments, G_VARIANT_TYPE_VARDICT) ||
      !g_variant_lookup (arguments, "action", "&s", &action) ||
      !g_strv_contains (supported, action))
    return NULL;

  MetaBackend *backend = meta_context_get_backend (meta_display_get_context (display));
  if (meta_backend_get_gnoblin_session_lock_active (backend))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "window management is unavailable while the session is locked");
      return NULL;
    }

  window_value = g_variant_lookup_value (arguments, "window", NULL);
  if (window_value)
    {
      if (!g_variant_is_of_type (window_value, G_VARIANT_TYPE_STRING))
        goto missing_window;
      selector = g_variant_get_string (window_value, NULL);
    }
  if (g_str_equal (selector, "active"))
    window = meta_display_get_focus_window (display);
  else
    {
      char *end = NULL;
      guint64 sequence = g_ascii_strtoull (selector, &end, 10);
      if (end == selector || *end || sequence > G_MAXUINT32)
        goto missing_window;
      windows = meta_display_list_all_windows (display);
      for (GList *item = windows; item; item = item->next)
        {
          MetaWindow *candidate = item->data;
          if (meta_window_get_stable_sequence (candidate) == sequence)
            {
              window = candidate;
              break;
            }
        }
    }

  if (!window || meta_window_is_skip_taskbar (window) ||
      meta_window_is_override_redirect (window))
    goto missing_window;

  stable_sequence = meta_window_get_stable_sequence (window);
  if (g_str_equal (action, "monitor"))
    {
      gint64 monitor;

      if (!g_variant_lookup (arguments, "monitor", "x", &monitor) ||
          monitor < 0 || monitor >= meta_display_get_n_monitors (display))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "monitor not found");
          return NULL;
        }
      meta_window_move_to_monitor (window, (int) monitor);
    }
  else if (g_str_equal (action, "close"))
    {
      if (!meta_window_can_close (window))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "window cannot be closed");
          return NULL;
        }
      meta_window_delete (window, meta_display_get_current_time (display));
    }
  else if (g_str_equal (action, "minimize"))
    {
      if (!meta_window_can_minimize (window))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "window cannot be minimized");
          return NULL;
        }
      meta_window_minimize (window);
    }
  else if (g_str_equal (action, "restore"))
    meta_window_unminimize (window);
  else if (g_str_equal (action, "maximize"))
    {
      if (!meta_window_can_maximize (window))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "window cannot be maximized");
          return NULL;
        }
      meta_window_maximize (window);
    }
  else if (g_str_equal (action, "unmaximize"))
    meta_window_unmaximize (window);
  else if (g_str_equal (action, "fullscreen"))
    meta_window_make_fullscreen (window);
  else if (g_str_equal (action, "unfullscreen"))
    meta_window_unmake_fullscreen (window);
  else if (g_str_equal (action, "above"))
    meta_window_make_above (window);
  else if (g_str_equal (action, "unabove"))
    meta_window_unmake_above (window);
  else if (g_str_equal (action, "stick"))
    meta_window_stick (window);
  else
    meta_window_unstick (window);

  g_variant_builder_init (&result, G_VARIANT_TYPE_VARDICT);
  window_id = g_strdup_printf ("%u", stable_sequence);
  g_variant_builder_add (&result, "{sv}", "ok", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&result, "{sv}", "pending", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&result, "{sv}", "window", g_variant_new_string (window_id));
  g_variant_builder_add (&result, "{sv}", "action", g_variant_new_string (action));
  return g_variant_ref_sink (g_variant_builder_end (&result));

missing_window:
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "window no longer available; list windows first");
  return NULL;
}


GVariant *
meta_gnoblin_dispatch_native_api (MetaDisplay *display,
                                  const char  *method,
                                  GVariant    *arguments,
                                  GError     **error)
{
  GVariantBuilder result;
  GVariantBuilder monitors;
  int n_monitors;
  int primary;

  g_return_val_if_fail (META_IS_DISPLAY (display), NULL);

  if (g_strcmp0 (method, "window.action") == 0)
    return gnoblin_native_window_action (display, arguments, error);
  if (g_strcmp0 (method, "window.list") == 0)
    return gnoblin_native_window_list (display, arguments, error);
  if (g_strcmp0 (method, "monitor.list") != 0)
    return NULL;

  g_return_val_if_fail (META_IS_DISPLAY (display), NULL);

  n_monitors = meta_display_get_n_monitors (display);
  primary = meta_display_get_primary_monitor (display);
  g_variant_builder_init (&monitors, G_VARIANT_TYPE ("aa{sv}"));
  for (int index = 0; index < n_monitors; index++)
    {
      GVariantBuilder monitor;
      MtkRectangle geometry;

      meta_display_get_monitor_geometry (display, index, &geometry);
      g_variant_builder_init (&monitor, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&monitor, "{sv}", "id", g_variant_new_int32 (index));
      g_variant_builder_add (&monitor, "{sv}", "x", g_variant_new_int32 (geometry.x));
      g_variant_builder_add (&monitor, "{sv}", "y", g_variant_new_int32 (geometry.y));
      g_variant_builder_add (&monitor, "{sv}", "width", g_variant_new_int32 (geometry.width));
      g_variant_builder_add (&monitor, "{sv}", "height", g_variant_new_int32 (geometry.height));
      g_variant_builder_add (&monitor, "{sv}", "primary", g_variant_new_boolean (index == primary));
      g_variant_builder_add (&monitor, "{sv}", "scale",
                             g_variant_new_double (meta_display_get_monitor_scale (display, index)));
      g_variant_builder_add_value (&monitors, g_variant_builder_end (&monitor));
    }

  g_variant_builder_init (&result, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&result, "{sv}", "monitors", g_variant_builder_end (&monitors));
  return g_variant_ref_sink (g_variant_builder_end (&result));
}
