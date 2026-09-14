#pragma once

#include "clutter/clutter-backend-private.h"
#include "clutter/clutter-settings.h"

G_BEGIN_DECLS

void    _clutter_settings_set_backend           (ClutterSettings *settings,
                                                 ClutterBackend  *backend);

CLUTTER_EXPORT
void    _clutter_settings_set_gnoblin_mouse_options (ClutterSettings *settings,
                                                     gboolean         double_click_configured,
                                                     int              double_click_time,
                                                     gboolean         drag_threshold_configured,
                                                     int              drag_threshold);

void clutter_settings_ensure_pointer_a11y_settings (ClutterSettings *settings,
                                                    ClutterSeat     *seat);

G_END_DECLS
