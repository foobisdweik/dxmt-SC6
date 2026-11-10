/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#include "wsi_window.hpp"
#include "wsi_monitor.hpp"

#include "util_string.hpp"
#include "log/log.hpp"

namespace mullion::wsi {

void getWindowSize(HWND hWindow, uint32_t *pWidth, uint32_t *pHeight) {
  if (pWidth)
    *pWidth = 1024;

  if (pHeight)
    *pHeight = 1024;
}

bool resizeWindow(HWND hWindow, MullionWindowState *pState, uint32_t width,
                  uint32_t height) {
  return false;
}

bool setWindowMode(HMONITOR hMonitor, HWND hWindow, const WsiMode &mode) {
  return false;
}

bool enterFullscreenMode(HMONITOR hMonitor, HWND hWindow,
                         MullionWindowState *pState,
                         [[maybe_unused]] bool modeSwitch) {
  return false;
}

bool leaveFullscreenMode(HWND hWindow, MullionWindowState *pState,
                         bool restoreCoordinates) {
  return false;
}

bool restoreDisplayMode(HMONITOR hMonitor) {
  return false;
}

HMONITOR getWindowMonitor(HWND hWindow) {
  return 0;
}

bool isWindow(HWND hWindow) { return true; }

bool updateFullscreenWindow(HMONITOR hMonitor, HWND hWindow, MullionWindowState *pState,
                            bool forceTopmost) {
  return false;
}

bool isForeground(HWND hWindow) { return true; }

bool isMinimized(HWND hWindow) { return false; }

} // namespace mullion::wsi
