#pragma once
#include "airconv_public.h"

namespace mullion {

// SC6 fork: whether the converter leaves out the path `name`, which is what MULLION_AIRCONV_SKIP (or
// DXMT_AIRCONV_SKIP) names it. A path is left out when that variable names it, or when the compilation running on this
// thread has `flag` among its shader flags (AirconvSkipScope). The variable is read once per process
bool airconv_skipped(const char *name, SM50_SHADER_FLAG flag);

// SC6 fork: makes the shader flags of one compilation visible to airconv_skipped for as long as it lives. A
// compilation runs on one thread, from its entry point (SM50Compile*) to the end of its optimization passes
class AirconvSkipScope {
public:
  explicit AirconvSkipScope(const SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs);
  ~AirconvSkipScope();
  AirconvSkipScope(const AirconvSkipScope &) = delete;
  AirconvSkipScope &operator=(const AirconvSkipScope &) = delete;

private:
  SM50_SHADER_FLAG previous_;
};

} // namespace mullion
