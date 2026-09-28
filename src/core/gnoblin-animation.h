/* Gnoblin-owned native compositor animation API. */
#pragma once

#include "meta/meta-window-actor.h"
#include "core/gnoblin-native-control.h"
#include "meta/util.h"

void meta_gnoblin_animation_window_mapped (MetaWindowActor *window_actor);
GVariant * meta_gnoblin_animation_dispatch (MetaDisplay *display,
                                             const char  *method,
                                             GVariant    *arguments,
                                             GError     **error);
