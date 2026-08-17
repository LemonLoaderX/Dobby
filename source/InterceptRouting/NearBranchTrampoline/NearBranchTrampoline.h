#pragma once

#include "dobby/dobby_internal.h"

#include "InterceptRouting/RoutingPlugin.h"

class NearBranchTrampolinePlugin : public RoutingPluginInterface {};

inline bool g_enable_near_trampoline = false;
inline bool g_require_near_trampoline = false;

PUBLIC extern "C" inline void dobby_set_near_trampoline(bool enable) {
  g_enable_near_trampoline = enable;
  g_require_near_trampoline = false;
}

PUBLIC extern "C" inline void dobby_set_near_trampoline_required(bool require) {
  g_enable_near_trampoline = require;
  g_require_near_trampoline = require;
}
