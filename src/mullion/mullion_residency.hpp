#pragma once
#include "winemetal.h"
#include <cstdint>
#include "ftl.hpp"

namespace mullion {

enum MULLION_RESOURCE_RESIDENCY : uint32_t {
  MULLION_RESOURCE_RESIDENCY_NULL = 0,
  MULLION_RESOURCE_RESIDENCY_VERTEX_READ = 1 << 0,
  MULLION_RESOURCE_RESIDENCY_VERTEX_WRITE = 1 << 1,
  MULLION_RESOURCE_RESIDENCY_FRAGMENT_READ = 1 << 2,
  MULLION_RESOURCE_RESIDENCY_FRAGMENT_WRITE = 1 << 3,
  MULLION_RESOURCE_RESIDENCY_OBJECT_READ = 1 << 4,
  MULLION_RESOURCE_RESIDENCY_OBJECT_WRITE = 1 << 5,
  MULLION_RESOURCE_RESIDENCY_MESH_READ = 1 << 6,
  MULLION_RESOURCE_RESIDENCY_MESH_WRITE = 1 << 7,
  MULLION_RESOURCE_RESIDENCY_MESH_GS_READ = 1 << 8,
  MULLION_RESOURCE_RESIDENCY_MESH_GS_WRITE = 1 << 9,
  MULLION_RESOURCE_RESIDENCY_OBJECT_GS_READ = 1 << 10,
  MULLION_RESOURCE_RESIDENCY_OBJECT_GS_WRITE = 1 << 11,
  MULLION_RESOURCE_RESIDENCY_READ = MULLION_RESOURCE_RESIDENCY_VERTEX_READ | MULLION_RESOURCE_RESIDENCY_FRAGMENT_READ |
                                 MULLION_RESOURCE_RESIDENCY_OBJECT_READ | MULLION_RESOURCE_RESIDENCY_MESH_READ |
                                 MULLION_RESOURCE_RESIDENCY_MESH_GS_READ | MULLION_RESOURCE_RESIDENCY_OBJECT_GS_READ,
  MULLION_RESOURCE_RESIDENCY_WRITE = MULLION_RESOURCE_RESIDENCY_VERTEX_WRITE | MULLION_RESOURCE_RESIDENCY_FRAGMENT_WRITE |
                                  MULLION_RESOURCE_RESIDENCY_OBJECT_WRITE | MULLION_RESOURCE_RESIDENCY_MESH_WRITE |
                                  MULLION_RESOURCE_RESIDENCY_MESH_GS_WRITE | MULLION_RESOURCE_RESIDENCY_OBJECT_GS_WRITE,
};

struct MULLION_RESOURCE_RESIDENCY_STATE {
  uint64_t last_encoder_id = 0;
  MULLION_RESOURCE_RESIDENCY last_residency_mask = MULLION_RESOURCE_RESIDENCY_NULL;
};

constexpr bool
CheckResourceResidency(MULLION_RESOURCE_RESIDENCY_STATE &state, uint64_t encoder_id, MULLION_RESOURCE_RESIDENCY &requested) {
  if (encoder_id > state.last_encoder_id) {
    state.last_residency_mask = requested;
    state.last_encoder_id = encoder_id;
    requested = state.last_residency_mask;
    return true;
  }
  if (encoder_id == state.last_encoder_id) {
    if ((state.last_residency_mask & requested) == requested) {
      // it's already resident
      return false;
    }
    state.last_residency_mask = requested | state.last_residency_mask;
    requested = state.last_residency_mask;
    return true;
  }
  // invalid
  return false;
};

constexpr WMTResourceUsage
GetUsageFromResidencyMask(MULLION_RESOURCE_RESIDENCY mask) {
  return ((mask & MULLION_RESOURCE_RESIDENCY_READ) ? WMTResourceUsageRead : WMTResourceUsage(0)) |
         ((mask & MULLION_RESOURCE_RESIDENCY_WRITE) ? WMTResourceUsageWrite : WMTResourceUsage(0));
}

constexpr WMTRenderStages
GetStagesFromResidencyMask(MULLION_RESOURCE_RESIDENCY mask) {
  return ((mask & (MULLION_RESOURCE_RESIDENCY_FRAGMENT_READ | MULLION_RESOURCE_RESIDENCY_FRAGMENT_WRITE))
              ? WMTRenderStageFragment
              : WMTRenderStages(0)) |
         ((mask & (MULLION_RESOURCE_RESIDENCY_VERTEX_READ | MULLION_RESOURCE_RESIDENCY_VERTEX_WRITE))
              ? WMTRenderStageVertex
              : WMTRenderStages(0)) |
         ((mask & (MULLION_RESOURCE_RESIDENCY_OBJECT_READ | MULLION_RESOURCE_RESIDENCY_OBJECT_WRITE |
                   MULLION_RESOURCE_RESIDENCY_OBJECT_GS_READ | MULLION_RESOURCE_RESIDENCY_OBJECT_GS_WRITE))
              ? WMTRenderStageObject
              : WMTRenderStages(0)) |
         ((mask & (MULLION_RESOURCE_RESIDENCY_MESH_READ | MULLION_RESOURCE_RESIDENCY_MESH_WRITE |
                   MULLION_RESOURCE_RESIDENCY_MESH_GS_READ | MULLION_RESOURCE_RESIDENCY_MESH_GS_WRITE))
              ? WMTRenderStageMesh
              : WMTRenderStages(0));
}

} // namespace mullion
