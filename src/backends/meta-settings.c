/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2017 Red Hat
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

#include "config.h"

#include "backends/meta-settings-private.h"

#include <gio/gio.h>
#include <gio/gsettingsschema.h>
#include <math.h>

#include "backends/meta-backend-private.h"
#include "backends/meta-logical-monitor-private.h"
#include "backends/meta-monitor-manager-private.h"

#ifdef HAVE_WAYLAND
#include "core/gnoblin-native-control.h"
#endif

#ifndef XWAYLAND_GRAB_DEFAULT_ACCESS_RULES
# warning "XWAYLAND_GRAB_DEFAULT_ACCESS_RULES is not set"
# define  XWAYLAND_GRAB_DEFAULT_ACCESS_RULES ""
#endif

enum
{
  UI_SCALING_FACTOR_CHANGED,
  GLOBAL_SCALING_FACTOR_CHANGED,
  FONT_DPI_CHANGED,
  EXPERIMENTAL_FEATURES_CHANGED,
  PRIVACY_SCREEN_CHANGED,
  XWAYLAND_SCALING_FACTOR_CHANGED,

  N_SIGNALS
};

static GDebugKey experimental_feature_keys[] = {
  { "kms-modifiers", META_EXPERIMENTAL_FEATURE_KMS_MODIFIERS },
  { "autoclose-xwayland", META_EXPERIMENTAL_FEATURE_AUTOCLOSE_XWAYLAND },
};

static guint signals[N_SIGNALS];

struct _MetaSettings
{
  GObject parent;

  MetaBackend *backend;

  GSettings *interface_settings;
  GSettings *privacy_settings;
  GSettings *wayland_settings;
  GSettings *experimental_settings;

  int ui_scaling_factor;
  int global_scaling_factor;
  gboolean gnoblin_global_scaling_factor_overridden;
  int gnoblin_global_scaling_factor;

  int font_dpi;

  gboolean privacy_screen;
  gboolean privacy_screen_overridden;

  MetaExperimentalFeature experimental_features;
  gboolean experimental_features_overridden;

  gboolean xwayland_allow_grabs;
  GPtrArray *xwayland_grab_allow_list_patterns;
  GPtrArray *xwayland_grab_deny_list_patterns;

  /* A bitmask of MetaXwaylandExtension enum */
  int xwayland_disable_extensions;

  /* Whether Xwayland should allow X11 clients from different endianness */
  gboolean xwayland_allow_byte_swapped_clients;

  float xwayland_scaling_factor;
};

G_DEFINE_TYPE (MetaSettings, meta_settings, G_TYPE_OBJECT)

static int
calculate_ui_scaling_factor (MetaSettings *settings)
{
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (settings->backend);
  MetaLogicalMonitor *primary_logical_monitor;

  primary_logical_monitor =
    meta_monitor_manager_get_primary_logical_monitor (monitor_manager);
  if (!primary_logical_monitor)
    return 1;

  return (int) meta_logical_monitor_get_scale (primary_logical_monitor);
}

static gboolean
update_ui_scaling_factor (MetaSettings *settings)
{
  int ui_scaling_factor;

  if (meta_backend_is_stage_views_scaled (settings->backend))
    ui_scaling_factor = 1;
  else
    ui_scaling_factor = calculate_ui_scaling_factor (settings);

  if (settings->ui_scaling_factor != ui_scaling_factor)
    {
      settings->ui_scaling_factor = ui_scaling_factor;
      return TRUE;
    }
  else
    {
      return FALSE;
    }
}

void
meta_settings_update_ui_scaling_factor (MetaSettings *settings)
{
  if (update_ui_scaling_factor (settings))
    g_signal_emit (settings, signals[UI_SCALING_FACTOR_CHANGED], 0);
}

int
meta_settings_get_ui_scaling_factor (MetaSettings *settings)
{
  g_assert (settings->ui_scaling_factor != 0);

  return settings->ui_scaling_factor;
}

static gboolean
update_global_scaling_factor (MetaSettings *settings)
{
  int global_scaling_factor;

  global_scaling_factor =
    settings->gnoblin_global_scaling_factor_overridden
      ? settings->gnoblin_global_scaling_factor
      : (int) g_settings_get_uint (settings->interface_settings,
                                   "scaling-factor");

  if (settings->global_scaling_factor != global_scaling_factor)
    {
      settings->global_scaling_factor = global_scaling_factor;
      return TRUE;
    }
  else
    {
      return FALSE;
    }
}

gboolean
meta_settings_get_global_scaling_factor (MetaSettings *settings,
                                         int          *out_scaling_factor)
{
  if (settings->global_scaling_factor == 0)
    return FALSE;

  *out_scaling_factor = settings->global_scaling_factor;
  return TRUE;
}

void
meta_settings_apply_gnoblin_monitor_preferences (MetaSettings *settings,
                                                 GVariant     *preferences)
{
  g_autoptr (GVariant) scale_value = NULL;

  g_return_if_fail (META_IS_SETTINGS (settings));
  g_return_if_fail (!preferences ||
                    g_variant_is_of_type (preferences, G_VARIANT_TYPE_VARDICT));

  settings->gnoblin_global_scaling_factor_overridden = FALSE;
  if (preferences)
    scale_value = g_variant_lookup_value (preferences, "global-scale", NULL);

  if (scale_value &&
      g_variant_is_of_type (scale_value, G_VARIANT_TYPE_INT64))
    {
      gint64 requested_scale = g_variant_get_int64 (scale_value);

      if (requested_scale >= 0 && requested_scale <= 4)
        {
          settings->gnoblin_global_scaling_factor_overridden = TRUE;
          settings->gnoblin_global_scaling_factor = (int) requested_scale;
        }
      else
        {
          g_warning ("Ignoring invalid Gnoblin global monitor scale");
        }
    }
  else if (scale_value &&
           !(g_variant_is_of_type (scale_value, G_VARIANT_TYPE_STRING) &&
             g_str_equal (g_variant_get_string (scale_value, NULL), "inherit")))
    {
      g_warning ("Ignoring invalid Gnoblin global monitor scale");
    }

  if (update_global_scaling_factor (settings))
    g_signal_emit (settings, signals[GLOBAL_SCALING_FACTOR_CHANGED], 0);
}

static gboolean
update_font_dpi (MetaSettings *settings)
{
  ClutterContext *clutter_context;
  ClutterSettings *clutter_settings;
  double text_scaling_factor;
  /* Number of logical pixels on an inch when unscaled */
  const double dots_per_inch = 96;
  /* Being based on Xft, API users expect the DPI to be 1/1024th of an inch. */
  const double xft_factor = 1024;
  int font_dpi;

  text_scaling_factor = g_settings_get_double (settings->interface_settings,
                                               "text-scaling-factor");
  font_dpi = (int) (text_scaling_factor *
                    dots_per_inch *
                    xft_factor *
                    settings->ui_scaling_factor);

  if (font_dpi != settings->font_dpi)
    {
      settings->font_dpi = font_dpi;
      clutter_context = meta_backend_get_clutter_context (settings->backend);
      clutter_settings = clutter_context_get_settings (clutter_context);

      g_object_set (clutter_settings,
                    "font-dpi", font_dpi,
                    NULL);

      return TRUE;
    }
  else
    {
      return FALSE;
    }
}

static void
meta_settings_update_font_dpi (MetaSettings *settings)
{
  if (update_font_dpi (settings))
    g_signal_emit (settings, signals[FONT_DPI_CHANGED], 0);
}

int
meta_settings_get_font_dpi (MetaSettings *settings)
{
  g_assert (settings->font_dpi != 0);

  return settings->font_dpi;
}

static void
interface_settings_changed (GSettings    *interface_settings,
                            const char   *key,
                            MetaSettings *settings)
{
  if (g_str_equal (key, "scaling-factor"))
    {
      if (update_global_scaling_factor (settings))
        g_signal_emit (settings, signals[GLOBAL_SCALING_FACTOR_CHANGED], 0);
    }
  else if (g_str_equal (key, "text-scaling-factor"))
    {
      meta_settings_update_font_dpi (settings);
    }
}

static void
privacy_settings_changed (GSettings    *privacy_settings,
                          const char   *key,
                          MetaSettings *settings)
{
  if (g_str_equal (key, "privacy-screen"))
    {
      gboolean privacy_screen;

      if (settings->privacy_screen_overridden)
        return;

      privacy_screen = g_settings_get_boolean (privacy_settings, key);

      if (settings->privacy_screen != privacy_screen)
        {
          settings->privacy_screen = privacy_screen;
          g_signal_emit (settings, signals[PRIVACY_SCREEN_CHANGED], 0);
        }
    }
}

gboolean
meta_settings_is_experimental_feature_enabled (MetaSettings           *settings,
                                               MetaExperimentalFeature feature)
{
  return !!(settings->experimental_features & feature);
}

void
meta_settings_override_experimental_features (MetaSettings *settings)
{
  settings->experimental_features = META_EXPERIMENTAL_FEATURE_NONE;
  settings->experimental_features_overridden = TRUE;
}

void
meta_settings_enable_experimental_feature (MetaSettings           *settings,
                                           MetaExperimentalFeature feature)
{
  g_assert (settings->experimental_features_overridden);

  settings->experimental_features |= feature;
}

static void
update_experimental_feature (MetaSettings *settings,
                             const char   *key)
{
  GSettings *experimental_settings = settings->experimental_settings;
  MetaExperimentalFeature feature = META_EXPERIMENTAL_FEATURE_NONE;
  gboolean enabled;

  if (settings->experimental_features_overridden)
    return;

  if (g_str_equal (key, "kms-modifiers"))
    feature = META_EXPERIMENTAL_FEATURE_KMS_MODIFIERS;
  else if (g_str_equal (key, "autoclose-xwayland"))
    feature = META_EXPERIMENTAL_FEATURE_AUTOCLOSE_XWAYLAND;

  if (!feature)
    {
      g_warning ("Unknown experimental feature '%s'", key);
      return;
    }

  enabled = g_settings_get_boolean (experimental_settings, key);
  if (enabled)
    settings->experimental_features |= feature;
  else
    settings->experimental_features &= ~feature;
}

static void
experimental_settings_changed (GSettings    *experimental_settings,
                               const char   *key,
                               MetaSettings *settings)
{
  MetaExperimentalFeature old_experimental_features;

  old_experimental_features = settings->experimental_features;

  update_experimental_feature (settings, key);

  if (settings->experimental_features != old_experimental_features)
    g_signal_emit (settings, signals[EXPERIMENTAL_FEATURES_CHANGED], 0,
                   (unsigned int) old_experimental_features);
}

static void
xwayland_grab_list_add_item (MetaSettings *settings,
                             char         *item)
{
  /* If first character is '!', it's a denied value */
  if (item[0] != '!')
    g_ptr_array_add (settings->xwayland_grab_allow_list_patterns,
                     g_pattern_spec_new (item));
  else if (item[1] != 0)
    g_ptr_array_add (settings->xwayland_grab_deny_list_patterns,
                     g_pattern_spec_new (&item[1]));
}

static void
reset_xwayland_grab_access_rules (MetaSettings *settings)
{
  g_auto (GStrv) system_defaults = NULL;
  int i;

  g_clear_pointer (&settings->xwayland_grab_allow_list_patterns,
                   g_ptr_array_unref);
  settings->xwayland_grab_allow_list_patterns =
    g_ptr_array_new_with_free_func ((GDestroyNotify) g_pattern_spec_free);

  g_clear_pointer (&settings->xwayland_grab_deny_list_patterns,
                   g_ptr_array_unref);
  settings->xwayland_grab_deny_list_patterns =
    g_ptr_array_new_with_free_func ((GDestroyNotify) g_pattern_spec_free);

  system_defaults = g_strsplit (XWAYLAND_GRAB_DEFAULT_ACCESS_RULES, ",", -1);
  for (i = 0; system_defaults[i]; i++)
    xwayland_grab_list_add_item (settings, system_defaults[i]);
}

static GVariant *
xwayland_array_item (GVariant *array,
                     gsize     index)
{
  GVariant *item = g_variant_get_child_value (array, index);

  if (g_variant_is_of_type (item, G_VARIANT_TYPE_VARIANT))
    {
      GVariant *unboxed = g_variant_get_variant (item);
      g_variant_unref (item);
      return unboxed;
    }

  return item;
}

static gboolean
xwayland_grab_access_rules_handler (GVariant *variant,
                                    gpointer *result,
                                    gpointer  data)
{
  MetaSettings *settings = data;
  GVariantIter iter;
  char *item;

  /* Create a GPatternSpec for each element */
  g_variant_iter_init (&iter, variant);
  while (g_variant_iter_loop (&iter, "s", &item))
    xwayland_grab_list_add_item (settings, item);

  *result = GINT_TO_POINTER (TRUE);

  return TRUE;
}

static void
update_xwayland_grab_access_rules (MetaSettings *settings)
{
  reset_xwayland_grab_access_rules (settings);

  /* Then add gsettings values */
  g_settings_get_mapped (settings->wayland_settings,
                         "xwayland-grab-access-rules",
                         xwayland_grab_access_rules_handler,
                         settings);
}

static void
update_xwayland_allow_grabs (MetaSettings *settings)
{
  settings->xwayland_allow_grabs =
    g_settings_get_boolean (settings->wayland_settings,
                            "xwayland-allow-grabs");
}

static void
update_xwayland_disable_extensions (MetaSettings *settings)
{
  settings->xwayland_disable_extensions =
    g_settings_get_flags (settings->wayland_settings,
                          "xwayland-disable-extension");
}

static void
update_privacy_settings (MetaSettings *settings)
{
  privacy_settings_changed (settings->privacy_settings,
                            "privacy-screen",
                            settings);
}

static void
update_xwayland_allow_byte_swapped_clients (MetaSettings *settings)
{
  settings->xwayland_allow_byte_swapped_clients =
    g_settings_get_boolean (settings->wayland_settings,
                            "xwayland-allow-byte-swapped-clients");
}

static void
update_xwayland_scaling_factor (MetaSettings *settings)
{
  settings->xwayland_scaling_factor =
    (float) g_settings_get_double (settings->wayland_settings,
                                   "xwayland-scaling-factor");
}

#ifdef HAVE_WAYLAND
static gboolean
is_gnoblin_session (void)
{
  return gnoblin_native_control_is_session (NULL);
}
#else
static gboolean
is_gnoblin_session (void)
{
  return FALSE;
}
#endif

static gboolean
gnoblin_owns_xwayland_preference (const char *key)
{
  return g_str_equal (key, "xwayland-allow-grabs") ||
         g_str_equal (key, "xwayland-grab-access-rules") ||
         g_str_equal (key, "xwayland-disable-extension") ||
         g_str_equal (key, "xwayland-allow-byte-swapped-clients") ||
         g_str_equal (key, "xwayland-scaling-factor");
}

static gboolean
variant_get_number (GVariant *value,
                    double   *number)
{
  if (g_variant_is_of_type (value, G_VARIANT_TYPE_DOUBLE))
    *number = g_variant_get_double (value);
  else if (g_variant_is_of_type (value, G_VARIANT_TYPE_INT64))
    *number = (double) g_variant_get_int64 (value);
  else if (g_variant_is_of_type (value, G_VARIANT_TYPE_INT32))
    *number = (double) g_variant_get_int32 (value);
  else
    return FALSE;

  return isfinite (*number);
}

void
meta_settings_apply_gnoblin_xwayland_preferences (MetaSettings *settings,
                                                  GVariant     *preferences)
{
  g_autoptr (GVariant) rules = NULL;
  g_autoptr (GVariant) extensions = NULL;
  g_autoptr (GVariant) scale_value = NULL;
  gboolean allow_grabs = FALSE;
  gboolean allow_byte_swapped_clients = FALSE;
  gboolean scaling_factor_changed;
  double scaling_factor = 0.0;
  int disable_extensions = 0;

  g_return_if_fail (META_IS_SETTINGS (settings));
  g_return_if_fail (!preferences ||
                    g_variant_is_of_type (preferences, G_VARIANT_TYPE_VARDICT));
  g_return_if_fail (is_gnoblin_session ());

  if (preferences)
    {
      g_variant_lookup (preferences, "allow-grabs", "b", &allow_grabs);
      g_variant_lookup (preferences, "allow-byte-swapped-clients", "b",
                        &allow_byte_swapped_clients);
      rules = g_variant_lookup_value (preferences, "grab-access-rules", NULL);
      extensions = g_variant_lookup_value (preferences, "disable-extensions", NULL);
      scale_value = g_variant_lookup_value (preferences, "scaling-factor", NULL);
      if (scale_value && !variant_get_number (scale_value, &scaling_factor))
        g_warning ("Ignoring invalid Gnoblin Xwayland scaling factor");
    }

  settings->xwayland_allow_grabs = allow_grabs;
  settings->xwayland_allow_byte_swapped_clients = allow_byte_swapped_clients;

  reset_xwayland_grab_access_rules (settings);
  if (rules &&
      (g_variant_is_of_type (rules, G_VARIANT_TYPE ("av")) ||
       g_variant_is_of_type (rules, G_VARIANT_TYPE_STRING_ARRAY)))
    {
      for (gsize i = 0; i < g_variant_n_children (rules); i++)
        {
          g_autoptr (GVariant) item = xwayland_array_item (rules, i);

          if (g_variant_is_of_type (item, G_VARIANT_TYPE_STRING))
            xwayland_grab_list_add_item (
              settings, (char *) g_variant_get_string (item, NULL));
        }
    }

  if (extensions &&
      (g_variant_is_of_type (extensions, G_VARIANT_TYPE ("av")) ||
       g_variant_is_of_type (extensions, G_VARIANT_TYPE_STRING_ARRAY)))
    {
      for (gsize i = 0; i < g_variant_n_children (extensions); i++)
        {
          g_autoptr (GVariant) item = xwayland_array_item (extensions, i);
          const char *name;

          if (!g_variant_is_of_type (item, G_VARIANT_TYPE_STRING))
            continue;

          name = g_variant_get_string (item, NULL);
          if (g_str_equal (name, "security"))
            disable_extensions |= META_XWAYLAND_EXTENSION_SECURITY;
          else if (g_str_equal (name, "xtest"))
            disable_extensions |= META_XWAYLAND_EXTENSION_XTEST;
        }
    }
  settings->xwayland_disable_extensions = disable_extensions;

  scaling_factor_changed =
    settings->xwayland_scaling_factor != (float) scaling_factor;
  settings->xwayland_scaling_factor = (float) scaling_factor;
  if (scaling_factor_changed)
    g_signal_emit (settings, signals[XWAYLAND_SCALING_FACTOR_CHANGED], 0);
}

static void
wayland_settings_changed (GSettings    *wayland_settings,
                          gchar        *key,
                          MetaSettings *settings)
{
  if (is_gnoblin_session () && gnoblin_owns_xwayland_preference (key))
    return;

  if (g_str_equal (key, "xwayland-allow-grabs"))
    {
      update_xwayland_allow_grabs (settings);
    }
  else if (g_str_equal (key, "xwayland-grab-access-rules"))
    {
      update_xwayland_grab_access_rules (settings);
    }
  else if (g_str_equal (key, "xwayland-disable-extension"))
    {
      update_xwayland_disable_extensions (settings);
    }
  else if (g_str_equal (key, "xwayland-allow-byte-swapped-clients"))
    {
      update_xwayland_allow_byte_swapped_clients (settings);
    }
  else if (g_str_equal (key, "xwayland-scaling-factor"))
    {
      update_xwayland_scaling_factor (settings);
      g_signal_emit (settings, signals[XWAYLAND_SCALING_FACTOR_CHANGED], 0);
    }
}

void
meta_settings_get_xwayland_grab_patterns (MetaSettings  *settings,
                                          GPtrArray    **allow_list_patterns,
                                          GPtrArray    **deny_list_patterns)
{
  *allow_list_patterns = settings->xwayland_grab_allow_list_patterns;
  *deny_list_patterns = settings->xwayland_grab_deny_list_patterns;
}

gboolean
meta_settings_are_xwayland_grabs_allowed (MetaSettings *settings)
{
  return (settings->xwayland_allow_grabs);
}

int
meta_settings_get_xwayland_disable_extensions (MetaSettings *settings)
{
  return (settings->xwayland_disable_extensions);
}

gboolean
meta_settings_are_xwayland_byte_swapped_clients_allowed (MetaSettings *settings)
{
  return settings->xwayland_allow_byte_swapped_clients;
}

gboolean
meta_settings_get_xwayland_scaling_factor (MetaSettings *settings,
                                           float        *scaling_factor)
{
  if (G_APPROX_VALUE (settings->xwayland_scaling_factor, 0.0f, FLT_EPSILON))
    {
      return FALSE;
    }
  else
    {
      *scaling_factor = settings->xwayland_scaling_factor;
      return TRUE;
    }
}

gboolean
meta_settings_is_privacy_screen_enabled (MetaSettings *settings)
{
  return settings->privacy_screen;
}

void
meta_settings_set_privacy_screen_enabled (MetaSettings *settings,
                                          gboolean      enabled)
{
  if (settings->privacy_screen == enabled)
    return;

  settings->privacy_screen = enabled;

  if (!settings->privacy_screen_overridden)
    g_settings_set_boolean (settings->privacy_settings, "privacy-screen",
                            enabled);
}

void
meta_settings_apply_gnoblin_privacy_screen_preferences (MetaSettings *settings,
                                                        GVariant     *preferences)
{
  gboolean overridden = FALSE;
  gboolean enabled = FALSE;

  if (preferences)
    overridden = g_variant_lookup (preferences, "privacy-screen", "b", &enabled);

  settings->privacy_screen_overridden = overridden;

  if (!overridden)
    enabled = g_settings_get_boolean (settings->privacy_settings,
                                      "privacy-screen");

  if (settings->privacy_screen != enabled)
    {
      settings->privacy_screen = enabled;
      g_signal_emit (settings, signals[PRIVACY_SCREEN_CHANGED], 0);
    }
}

MetaSettings *
meta_settings_new (MetaBackend *backend)
{
  MetaSettings *settings;

  settings = g_object_new (META_TYPE_SETTINGS, NULL);
  settings->backend = backend;

  if (is_gnoblin_session ())
    {
#ifdef HAVE_WAYLAND
      g_autoptr (GVariant) document =
        gnoblin_native_control_get_config_document (NULL);
      g_autoptr (GVariant) preferences = document
        ? g_variant_lookup_value (document, "xwayland", G_VARIANT_TYPE_VARDICT)
        : NULL;

      meta_settings_apply_gnoblin_xwayland_preferences (settings, preferences);
#endif
    }

  return settings;
}

static void
meta_settings_dispose (GObject *object)
{
  MetaSettings *settings = META_SETTINGS (object);

  g_clear_object (&settings->interface_settings);
  g_clear_object (&settings->privacy_settings);
  g_clear_object (&settings->wayland_settings);
  g_clear_object (&settings->experimental_settings);
  g_clear_pointer (&settings->xwayland_grab_allow_list_patterns,
                   g_ptr_array_unref);
  g_clear_pointer (&settings->xwayland_grab_deny_list_patterns,
                   g_ptr_array_unref);

  G_OBJECT_CLASS (meta_settings_parent_class)->dispose (object);
}

static void
meta_settings_init (MetaSettings *settings)
{
  g_autoptr (GSettingsSchema) experimental_schema = NULL;
  g_auto (GStrv) experimental_keys = NULL;
  const char *experimental_features_env;
  int i;

  settings->interface_settings = g_settings_new ("org.gnome.desktop.interface");
  g_signal_connect (settings->interface_settings, "changed",
                    G_CALLBACK (interface_settings_changed),
                    settings);
  settings->privacy_settings = g_settings_new ("org.gnome.desktop.privacy");
  g_signal_connect (settings->privacy_settings, "changed",
                    G_CALLBACK (privacy_settings_changed),
                    settings);
  settings->wayland_settings = g_settings_new ("org.gnome.mutter.wayland");
  g_signal_connect (settings->wayland_settings, "changed",
                    G_CALLBACK (wayland_settings_changed),
                    settings);
  experimental_schema =
    g_settings_schema_source_lookup (g_settings_schema_source_get_default (),
                                     "org.gnome.mutter.experimental", TRUE);
  settings->experimental_settings =
    g_settings_new_full (experimental_schema, NULL, NULL);
  g_signal_connect (settings->experimental_settings, "changed",
                    G_CALLBACK (experimental_settings_changed),
                    settings);

  /* Chain up inter-dependent settings. */
  g_signal_connect (settings, "global-scaling-factor-changed",
                    G_CALLBACK (meta_settings_update_ui_scaling_factor), NULL);
  g_signal_connect (settings, "ui-scaling-factor-changed",
                    G_CALLBACK (meta_settings_update_font_dpi), NULL);

  experimental_features_env = getenv ("MUTTER_DEBUG_EXPERIMENTAL_FEATURES");
  if (experimental_features_env)
    {
      MetaExperimentalFeature experimental_features;

      experimental_features =
        g_parse_debug_string (experimental_features_env,
                              experimental_feature_keys,
                              G_N_ELEMENTS (experimental_feature_keys));

      meta_settings_override_experimental_features (settings);
      meta_settings_enable_experimental_feature (settings,
                                                 experimental_features);
    }

  experimental_keys = g_settings_schema_list_keys (experimental_schema);
  for (i = 0; experimental_keys[i]; i++)
    update_experimental_feature (settings, experimental_keys[i]);

  update_global_scaling_factor (settings);
  update_xwayland_grab_access_rules (settings);
  update_xwayland_allow_grabs (settings);
  update_xwayland_disable_extensions (settings);
  update_privacy_settings (settings);
  update_xwayland_allow_byte_swapped_clients (settings);
  update_xwayland_scaling_factor (settings);
}

static void
on_monitors_changed (MetaMonitorManager *monitor_manager,
                     MetaSettings       *settings)
{
  meta_settings_update_ui_scaling_factor (settings);
}

void
meta_settings_post_init (MetaSettings *settings)
{
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (settings->backend);

  update_ui_scaling_factor (settings);
  update_font_dpi (settings);

  g_signal_connect_object (monitor_manager, "monitors-changed-internal",
                           G_CALLBACK (on_monitors_changed),
                           settings, G_CONNECT_AFTER);
}

static void
meta_settings_class_init (MetaSettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = meta_settings_dispose;

  signals[UI_SCALING_FACTOR_CHANGED] =
    g_signal_new ("ui-scaling-factor-changed",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[GLOBAL_SCALING_FACTOR_CHANGED] =
    g_signal_new ("global-scaling-factor-changed",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[FONT_DPI_CHANGED] =
    g_signal_new ("font-dpi-changed",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[EXPERIMENTAL_FEATURES_CHANGED] =
    g_signal_new ("experimental-features-changed",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_UINT);

  signals[PRIVACY_SCREEN_CHANGED] =
    g_signal_new ("privacy-screen-changed",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[XWAYLAND_SCALING_FACTOR_CHANGED] =
    g_signal_new ("xwayland-scaling-factor-changed",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}
