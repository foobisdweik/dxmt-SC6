#include "airconv_switches.hpp"
#include <cstdlib>
#include <cstring>

namespace mullion {

// the flags of the compilation that runs on this thread, or none outside one
static thread_local SM50_SHADER_FLAG current_flags = {};

bool
airconv_skipped(const char *name, SM50_SHADER_FLAG flag) {
  if (current_flags & flag)
    return true;
  static const char *const names = [] {
    auto value = getenv("MULLION_AIRCONV_SKIP");
    return value ? value : getenv("DXMT_AIRCONV_SKIP");
  }();
  return names && strstr(names, name);
}

AirconvSkipScope::AirconvSkipScope(const SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs) : previous_(current_flags) {
  SM50_SHADER_FLAG flags = {};
  for (auto arg = pArgs; arg; arg = (const SM50_SHADER_COMPILATION_ARGUMENT_DATA *)arg->next) {
    if (arg->type == SM50_SHADER_COMMON) {
      flags = ((const SM50_SHADER_COMMON_DATA *)arg)->flags;
      break;
    }
  }
  current_flags = flags;
}

AirconvSkipScope::~AirconvSkipScope() {
  current_flags = previous_;
}

} // namespace mullion
