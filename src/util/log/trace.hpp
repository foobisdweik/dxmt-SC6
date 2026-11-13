#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "../util_string.hpp"

namespace mullion::trace {

// MULLION_TRACE_<CATEGORY> (docs/tracing.md), one variable a category: 1 logs what is out of the ordinary in it, at
// little cost; `heavy` logs every event of it and turns on what costs the GPU or the CPU noticeably. The lines go to
// mullion.log in MULLION_TRACE_DIR, or else ~/Library/Logs/Mullion/<executable>/
enum Category : uint32_t {
  Shader = 1u << 0,
  Present = 1u << 1,
  Memory = 1u << 2,
  Resource = 1u << 3,
  Sync = 1u << 4,
  Gpu = 1u << 5,
  Cpu = 1u << 6,
  Api = 1u << 7,
  Pass = 1u << 8,
};

struct Mask {
  uint32_t on = 0, heavy = 0;
};

// the categories the MULLION_TRACE_<CATEGORY> variables turn on, read once
const Mask &Get();
// how a MULLION_TRACE_<CATEGORY> value turns its category on: not (0 or empty), on (1), or heavy
enum class Level { Off, On, Heavy };
Level ParseLevel(const std::string &value);
const char *Name(Category category);

inline bool
On(Category category) {
  return Get().on & category;
}

inline bool
Heavy(Category category) {
  return Get().heavy & category;
}

inline bool
Any() {
  return Get().on != 0;
}

// the folder of the trace log, as a Unix path (MULLION_TRACE_DIR, or ~/Library/Logs/Mullion/<executable>), and as a
// Windows path to it; empty when there is no home folder to find it by
std::string Directory();
std::string WindowsDirectory();

// the message's lines in the trace log, each tagged with the category and the calling thread
void Write(Category category, const std::string &message);
// a line of the log as it is (warnings and errors, which the trace log has while tracing)
void WriteLine(const std::string &line);
// a flight record's site, a printf format of up to four 64-bit integers, with its words, in the trace log
void WriteFlight(Category category, const char *format, const uint64_t words[4]);

// MULLION_TRACE_PRESENT: a swap chain's Presents, each with a PresentTrace for its call
struct Presents {
  std::chrono::steady_clock::time_point last;
  uint64_t count = 0;
};

// as the Present returns, its line: the time since the one before and the time in it. 1 has the ones 50 ms or more
// after the one before (a hitch), heavy every one
class PresentTrace {
  Presents *presents_ = nullptr;
  const void *swap_chain_;
  unsigned interval_, flags_;
  std::chrono::steady_clock::time_point start_;

public:
  PresentTrace(Presents &presents, const void *swap_chain, unsigned interval, unsigned flags) :
      swap_chain_(swap_chain), interval_(interval), flags_(flags) {
    if (On(Present))
      presents_ = &presents, start_ = std::chrono::steady_clock::now();
  }
  PresentTrace(const PresentTrace &) = delete;
  ~PresentTrace();
};

} // namespace mullion::trace

// a line in the trace log when its category is on: one check of a cached mask otherwise
#define TRACE_IN(category, ...)                                                                                       \
  do {                                                                                                                 \
    if (::mullion::trace::On(::mullion::trace::category))                                                             \
      ::mullion::trace::Write(::mullion::trace::category, str::format(__VA_ARGS__));                                  \
  } while (0)
