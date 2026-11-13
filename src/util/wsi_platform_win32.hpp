/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#pragma once

#include <windows.h>

namespace mullion::wsi {

/**
 * \brief Impl-dependent state
 */
struct MullionWindowState {
  LONG style = 0;
  LONG exstyle = 0;
  RECT rect = {0, 0, 0, 0};
};

} // namespace mullion::wsi