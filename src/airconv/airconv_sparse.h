#pragma once

#ifdef __METAL__
typedef ulong sm50_sparse_qword;
#else
#include <stdint.h>
typedef uint64_t sm50_sparse_qword;
#endif

/* a count followed by these headers and one qword per tile (1 mapped, 0 NULL), captured at submission */
struct SM50_SPARSE_BUFFER_MAPPING {
  sm50_sparse_qword address;
  sm50_sparse_qword tile_count;
  sm50_sparse_qword tile_bytes;
};
