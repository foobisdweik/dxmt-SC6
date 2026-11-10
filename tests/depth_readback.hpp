// boundary cases follow the readback kernel's actual shape; conversion expectations remain independent.
#pragma once
#include <cstdio>
#include <cstring>

inline bool
depth_readback_group(unsigned group[3]) {
  static const unsigned char commands[] = {
#embed "../src/mullion/mullion_command.cpp"
      , 0
  };
  const char *copy = strstr(reinterpret_cast<const char *>(commands), "DepthStencilBlitContext::copyFromTexture(");
  const char *threads = copy ? strstr(copy, "threadgroup_size = {") : nullptr;
  return threads && sscanf(threads, "threadgroup_size = {%u, %u, %u}", &group[0], &group[1], &group[2]) == 3 &&
         group[0] && group[1] && group[2];
}
