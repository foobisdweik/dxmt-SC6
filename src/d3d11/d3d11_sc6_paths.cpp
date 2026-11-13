#include "d3d11_sc6_paths.hpp"
#include "config/config.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include <array>
#include <atomic>
#include <mutex>
#include <string>

namespace mullion {

static const std::array<Sc6PathInfo, size_t(Sc6Path::Count)> kPaths = {{
    {"cbuffer-bounds", "mullion.cbufferBounds", SM50_SHADER_FLAG_SKIP_CBUFFER_BOUNDS},
    {"frac-floor", "mullion.fracFloor", SM50_SHADER_FLAG_SKIP_FRAC_FLOOR},
    {"sample-branch-merge", "mullion.sampleBranchMerge", SM50_SHADER_FLAG_SKIP_SAMPLE_BRANCH_MERGE},
    {"minmax-branch-merge", "mullion.minmaxBranchMerge", SM50_SHADER_FLAG_SKIP_MINMAX_BRANCH_MERGE},
    {"group-sync-fence", "mullion.groupSyncFence", SM50_SHADER_FLAG{}},
    {"gs-layered", "mullion.gsLayered", SM50_SHADER_FLAG{}},
    {"pipeline-archive", "mullion.pipelineArchive", SM50_SHADER_FLAG{}},
    {"anisotropic-gradients", "mullion.anisoGradients", SM50_SHADER_FLAG_SKIP_ANISO_GRADIENTS},
    {"lazy-sampler-emulation", "mullion.lazySamplerEmulation", SM50_SHADER_FLAG{}},
}};

const Sc6PathInfo &
Sc6PathDescription(Sc6Path path) {
  return kPaths[size_t(path)];
}

// whether the comma-separated `list` names `name` as a whole item, or is "all"
static bool
ListNames(const std::string &list, const std::string &name) {
  size_t begin = 0;
  while (begin <= list.size()) {
    size_t end = list.find(',', begin);
    if (end == std::string::npos)
      end = list.size();
    auto item = list.substr(begin, end - begin);
    while (!item.empty() && item.front() == ' ')
      item.erase(item.begin());
    while (!item.empty() && item.back() == ' ')
      item.pop_back();
    if (item == name || item == "all")
      return true;
    begin = end + 1;
  }
  return false;
}

static const std::array<bool, size_t(Sc6Path::Count)> &
EnabledPaths() {
  static std::array<bool, size_t(Sc6Path::Count)> enabled;
  static std::once_flag once;
  std::call_once(once, [] {
    auto &config = Config::getInstance();
    auto config_list = config.getOption<std::string>("mullion.disable", "");
    auto env_list = env::getEnvVar("MULLION_SC6_DISABLE");
    std::string off;
    for (size_t i = 0; i < kPaths.size(); i++) {
      auto &path = kPaths[i];
      enabled[i] = config.getOption<bool>(path.option, true) && !ListNames(config_list, path.name) &&
                   !ListNames(env_list, path.name);
      if (!enabled[i])
        off += off.empty() ? path.name : std::string(", ") + path.name;
    }
    // the older switch for the layered geometry shader path keeps working
    if (env::getEnvVar("MULLION_SC6_GS_LAYERED") == "0" && enabled[size_t(Sc6Path::GSLayered)]) {
      enabled[size_t(Sc6Path::GSLayered)] = false;
      off += off.empty() ? "gs-layered" : ", gs-layered";
    }
    if (!off.empty())
      WARN("SC6 paths turned off: ", off);
  });
  return enabled;
}

bool
Sc6PathEnabled(Sc6Path path) {
  return EnabledPaths()[size_t(path)];
}

SM50_SHADER_FLAG
Sc6SkipShaderFlags() {
  uint32_t flags = 0;
  for (size_t i = 0; i < kPaths.size(); i++)
    if (!EnabledPaths()[i])
      flags |= kPaths[i].skip_flag;
  return SM50_SHADER_FLAG(flags);
}

static std::atomic<bool> g_minmax_emulation_needed{false};

void
NoteSamplerNeedsMinMaxEmulation() {
  if (!g_minmax_emulation_needed.exchange(true))
    WARN("A sampler needs the minimum or maximum filter emulation; shaders made from now on include it");
}

SM50_SHADER_FLAG
SamplerEmulationsLeftOut() {
  if (!Sc6PathEnabled(Sc6Path::LazySamplerEmulation))
    return SM50_SHADER_FLAG{};
  uint32_t flags = SM50_SHADER_FLAG_NO_CUSTOM_BORDER;
  if (!g_minmax_emulation_needed.load(std::memory_order_relaxed))
    flags |= SM50_SHADER_FLAG_NO_MINMAX_FILTER;
  return SM50_SHADER_FLAG(flags);
}

} // namespace mullion
