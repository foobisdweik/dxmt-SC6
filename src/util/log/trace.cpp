#include "trace.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iostream>
#include <sstream>

#include "../util_env.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace mullion::trace {

namespace {

constexpr std::array<std::pair<const char *, Category>, 9> kCategories = {{
    {"shader", Shader},
    {"present", Present},
    {"memory", Memory},
    {"resource", Resource},
    {"sync", Sync},
    {"gpu", Gpu},
    {"cpu", Cpu},
    {"api", Api},
    {"pass", Pass},
}};

std::string
Trimmed(std::string text) {
  auto first = text.find_first_not_of(" \t");
  auto last = text.find_last_not_of(" \t");
  return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

uint32_t
ThreadId() {
#ifdef _WIN32
  return GetCurrentThreadId();
#else
  uint64_t id = 0;
  pthread_threadid_np(nullptr, &id);
  return uint32_t(id);
#endif
}

// the lines, through winemetal's bridge to the process's one log; stderr where there is no bridge (native builds).
// winemetal.dll is already loaded wherever Mullion is (every Mullion DLL imports it): the first check may come while a
// DLL initializes, where loading one is not safe
void
Send(const std::string &lines, const Mask &mask) {
#ifdef _WIN32
  using WriteFunction = bool (*)(const char *, const char *, uint64_t, bool, bool);
  static const WriteFunction write = [] {
    HMODULE winemetal = GetModuleHandleA("winemetal.dll");
    return winemetal ? reinterpret_cast<WriteFunction>(GetProcAddress(winemetal, "WMTTraceWrite")) : nullptr;
  }();
  if (write && write(Directory().c_str(), lines.data(), lines.size(), mask.heavy & Gpu, mask.on & Memory))
    return;
#endif
  std::cerr << lines;
}

} // namespace

Level
ParseLevel(const std::string &value) {
  std::string level = Trimmed(value);
  for (auto &c : level)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  if (level.empty() || level == "0" || level == "off")
    return Level::Off;
  return level == "heavy" ? Level::Heavy : Level::On;
}

const Mask &
Get() {
  static const Mask mask = [] {
    Mask read;
    std::string on;
    for (auto &[name, category] : kCategories) {
      std::string variable = "MULLION_TRACE_";
      for (const char *c = name; *c; c++)
        variable += char(std::toupper(static_cast<unsigned char>(*c)));
      auto level = ParseLevel(env::getEnvVar(variable.c_str()));
      if (level == Level::Off)
        continue;
      read.on |= category;
      if (level == Level::Heavy)
        read.heavy |= category;
      on += str::format(on.empty() ? "" : ", ", name, level == Level::Heavy ? " (heavy)" : "");
    }
    // the log opens with the first check of a process that traces, before the device and its queues are made
    if (read.on)
      Send(str::format("[trace] ", env::getExeBaseName(), ": ", on, '\n'), read);
    return read;
  }();
  return mask;
}

std::string
Directory() {
  static const std::string directory = [] {
    auto given = env::getEnvVar("MULLION_TRACE_DIR");
    if (!given.empty())
      return given;
    // Wine's processes have the Unix home as WINEHOMEDIR, \??\Z: and the path, not HOME
    auto home = env::getEnvVar("WINEHOMEDIR");
    if (home.rfind("\\??\\Z:", 0) == 0) {
      home = home.substr(6);
      std::replace(home.begin(), home.end(), '\\', '/');
    } else
      home = env::getEnvVar("HOME");
    return home.empty() ? std::string() : home + "/Library/Logs/Mullion/" + env::getExeName();
  }();
  return directory;
}

std::string
WindowsDirectory() {
  auto directory = Directory();
  return directory.empty() || directory[0] != '/' ? directory : "Z:" + directory;
}

const char *
Name(Category category) {
  for (auto &[name, known] : kCategories)
    if (known == category)
      return name;
  return "?";
}

void
Write(Category category, const std::string &message) {
  std::string lines, prefix = str::format("[", Name(category), "] ", ThreadId(), ": ");
  std::stringstream stream(message);
  for (std::string line; std::getline(stream, line, '\n');)
    lines += prefix + line + '\n';
  Send(lines, Get());
}

void
WriteLine(const std::string &line) {
  Send(line.empty() || line.back() != '\n' ? line + '\n' : line, Get());
}

void
WriteFlight(Category category, const char *format, const uint64_t words[4]) {
  char line[1024];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
  snprintf(line, sizeof(line), format, words[0], words[1], words[2], words[3]);
#pragma clang diagnostic pop
  Write(category, line);
}

PresentTrace::~PresentTrace() {
  if (!presents_)
    return;
  using ms = std::chrono::duration<double, std::milli>;
  auto end = std::chrono::steady_clock::now();
  double since = presents_->count ? ms(start_ - presents_->last).count() : 0;
  presents_->last = start_;
  presents_->count++;
  if (since < 50 && !Heavy(Present))
    return;
  Write(Present, str::format("swap chain ", swap_chain_, ": Present ", presents_->count, ", interval ", interval_,
                             ", flags ", flags_, ", ", since, " ms since the last, ", ms(end - start_).count(),
                             " ms in it"));
}

} // namespace mullion::trace
