#include "mullion_capture.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include <atomic>
#include <chrono>
#include <ctime>

#ifdef __WIN32
#include "windows.h"
#endif

namespace mullion {

namespace {

#ifdef _WIN32
// SC6 fork: read the controller's right trigger (R2) through XInput, so a capture can start during play without a
// keyboard. The struct matches XINPUT_STATE from xinput.h; it is declared here to avoid a link dependency.
struct Sc6XInputState {
  DWORD packet;
  WORD buttons;
  BYTE left_trigger;
  BYTE right_trigger;
  SHORT thumbs[4];
};
typedef DWORD(WINAPI *Sc6XInputGetState)(DWORD, Sc6XInputState *);

bool
sc6RightTriggerDown() {
  static Sc6XInputGetState get_state;
  static bool loaded;
  if (!loaded) {
    loaded = true;
    for (const char *name : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
      if (HMODULE mod = LoadLibraryA(name)) {
        get_state = (Sc6XInputGetState)(void *)GetProcAddress(mod, "XInputGetState");
        if (get_state)
          break;
      }
    }
    WARN("SC6 capture trigger: XInput ", get_state ? "loaded" : "not available");
  }
  if (!get_state)
    return false;
  for (DWORD i = 0; i < 4; i++) {
    Sc6XInputState st{};
    // 200 of 255 is a firm press, so a light touch does not start a capture.
    if (get_state(i, &st) == ERROR_SUCCESS && st.right_trigger > 200)
      return true;
  }
  return false;
}

// SC6 fork: play a WAV file named by an env var (a Windows path), so the player hears when a capture starts and when
// the trace is written. winmm is loaded at runtime to avoid a link dependency. SND_ASYNC returns at once, so the
// calling thread does not wait for the sound.
void
sc6PlayCaptureSound(const char *env_name) {
  typedef BOOL(WINAPI * PlaySoundAFn)(LPCSTR, HMODULE, DWORD);
  static PlaySoundAFn play;
  static bool loaded;
  if (!loaded) {
    loaded = true;
    if (HMODULE mod = LoadLibraryA("winmm.dll"))
      play = (PlaySoundAFn)(void *)GetProcAddress(mod, "PlaySoundA");
    WARN("SC6 capture sound: winmm ", play ? "loaded" : "not available");
  }
  auto path = env::getEnvVar(env_name);
  if (!play || path.empty())
    return;
  // 0x20000 = SND_FILENAME, 0x0001 = SND_ASYNC, 0x0002 = SND_NODEFAULT
  if (!play(path.c_str(), nullptr, 0x20000 | 0x0001 | 0x0002))
    WARN("SC6 capture sound: cannot play ", path);
}
#else
void
sc6PlayCaptureSound(const char *) {}
#endif

enum class NextAction {
  StartCapture,
  StopCapture,
  Nothing,
};

enum class State {
  Idle,
  WillCapture,
  Capturing,
};

struct Capture {
  const bool enabled = env::getEnvVar("MTL_CAPTURE_ENABLED") == "1" &&
                       env::getExeBaseName() == env::getEnvVar("MULLION_CAPTURE_EXECUTABLE");
  mullion::mutex mutex;
  State state = State::Idle;
  uint64_t next_capture_frame = 0;
  bool capture_key_pressed_ = false;
  // SC6 fork: R2 (which starts a Critical Edge) arms a capture that fires a delay later, inside the slow part of the
  // effect. Once per game launch.
  bool sc6_r2_pressed_ = false;
  bool sc6_r2_used_ = false;
  uint64_t sc6_r2_fire_ms_ = 0;
  unsigned holds = 0;
  mullion::thread stopping_;
  std::atomic<bool> stopped_ = true;

  Capture() {
    if (!enabled)
      return;
    WARN("Mullion capture enabled");
    try {
      if (auto frame = env::getEnvVar("MULLION_CAPTURE_FRAME"); !frame.empty())
        scheduleNextFrameCapture(std::stoull(frame));
    } catch (const std::exception &) {
    }
  }

  NextAction
  getNextAction(uint64_t frame) {
    switch (state) {
    case State::Idle:
      break;
    case State::WillCapture:
      if (frame == next_capture_frame) {
        state = State::Capturing;
        return NextAction::StartCapture;
      }
      break;
    case State::Capturing:
      if (frame > next_capture_frame) {
        next_capture_frame = 0;
        state = State::Idle;
        return NextAction::StopCapture;
      }
      break;
    }
    return NextAction::Nothing;
  }

  void
  scheduleNextFrameCapture(uint64_t next = 0) {
    if (state != State::Idle)
      return;
    state = State::WillCapture;
    next_capture_frame = next;
  }

  bool
  shouldCaptureNextFrame() {
#ifdef _WIN32
    if (state != State::Idle)
      return false;
    SHORT state_ = GetAsyncKeyState(VK_F10);
    if (state_ & 0x8000 && !capture_key_pressed_) {
      capture_key_pressed_ = true;
      return true;
    }
    capture_key_pressed_ = state_ & 0x8000;
    if (sc6_r2_fire_ms_ && GetTickCount64() >= sc6_r2_fire_ms_) {
      sc6_r2_fire_ms_ = 0;
      WARN("SC6 capture trigger: delay over, capturing next frame");
      return true;
    }
    if (!sc6_r2_used_) {
      bool r2 = sc6RightTriggerDown();
      if (r2 && !sc6_r2_pressed_) {
        sc6_r2_used_ = true;
        auto delay = env::getEnvVar("DXMT_SC6_CAPTURE_DELAY_MS");
        uint64_t delay_ms = 1500;
        try {
          if (!delay.empty())
            delay_ms = std::stoull(delay);
        } catch (const std::exception &) {
        }
        sc6_r2_fire_ms_ = GetTickCount64() + delay_ms;
        WARN("SC6 capture trigger: R2 pressed, capture armed");
      }
      sc6_r2_pressed_ = r2;
    }
#endif
    return false;
  }
};

// never destroyed: a hold the process never released must not meet a destroyed capture at its exit
Capture &
capture() {
  static auto &capture = *new Capture;
  return capture;
}

} // namespace

CaptureState::CaptureState() {
  auto &capture = mullion::capture();
  if (!capture.enabled)
    return;
  std::lock_guard lock(capture.mutex);
  capture.holds++;
}

CaptureState::~CaptureState() {
  auto &capture = mullion::capture();
  if (!capture.enabled)
    return;
  std::lock_guard lock(capture.mutex);
  if (--capture.holds)
    return;
  if (capture.state == State::Capturing) {
    capture.state = State::Idle;
    capture.next_capture_frame = 0;
    WMT::CaptureManager::sharedCaptureManager().stopCapture();
  }
  if (capture.stopping_.joinable())
    capture.stopping_.join();
}

void
CaptureState::frame(WMT::Device device, uint64_t frame) {
  auto &capture = mullion::capture();
  if (!capture.enabled)
    return;
  std::lock_guard lock(capture.mutex);
  switch (capture.getNextAction(frame)) {
  case NextAction::StartCapture: {
    // Metal takes one capture at a time, and the stop of the last one may wait for what this thread commits next
    if (!capture.stopped_) {
      capture.state = State::WillCapture;
      capture.next_capture_frame = frame + 1;
      break;
    }
    if (capture.stopping_.joinable())
      capture.stopping_.join();
    WMTCaptureInfo info;
    info.capture_object = device;
    info.destination = WMTCaptureDestinationGPUTraceDocument;
    char filename[1024];
    std::time_t now;
    std::time(&now);
    std::strftime(filename, 1024, "_%H'%M'%S_%m-%d-%y.gputrace", std::localtime(&now));
    // a trace is large: it goes where the logs go, not next to the executable, when MULLION_LOG_PATH names a directory
    auto directory = env::getEnvVar("MULLION_LOG_PATH");
    if (directory == "none")
      directory.clear();
    if (!directory.empty() && directory.back() != '/' && directory.back() != '\\')
      directory += '\\';
    auto fileUrl = env::getUnixPath(directory + env::getExeBaseName() + "_F." + std::to_string(frame) + filename);
    WARN("A new capture will be saved to ", fileUrl);
    info.output_url.set(fileUrl.c_str());
    WMT::CaptureManager::sharedCaptureManager().startCapture(info);
    sc6PlayCaptureSound("DXMT_SC6_CAPTURE_SOUND_START");
    break;
  }
  case NextAction::StopCapture:
    // Metal's stop waits for the device's work, which can wait for what the calling thread submits next
    capture.stopped_ = false;
    capture.stopping_ = mullion::thread([&capture] {
      // SC6 fork: the "done" sound plays after stopCapture returns; the logged time shows how long Metal took to
      // write the .gputrace
      auto t0 = std::chrono::steady_clock::now();
      WMT::CaptureManager::sharedCaptureManager().stopCapture();
      WARN("Capture stopped; stopCapture took ",
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), " ms");
      sc6PlayCaptureSound("DXMT_SC6_CAPTURE_SOUND_DONE");
      capture.stopped_ = true;
    });
    break;
  case NextAction::Nothing:
    if (capture.shouldCaptureNextFrame())
      capture.scheduleNextFrameCapture(frame + 1);
    break;
  }
}

} // namespace mullion
