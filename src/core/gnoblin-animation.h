/* Gnoblin-owned native compositor animation API. */
#pragma once

#include "meta/util.h"

GVariant * meta_gnoblin_animation_dispatch (MetaDisplay *display,
                                             const char  *method,
                                             GVariant    *arguments,
                                             GError     **error);
