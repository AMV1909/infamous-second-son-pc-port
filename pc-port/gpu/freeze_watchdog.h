// SPDX-License-Identifier: GPL-2.0-or-later
// Freeze watchdog: thread stacks into the session's log folder when frames stop (freeze_watchdog.cpp).
#pragma once
#include <cstdint>

namespace iss::freeze_watchdog {
void OnPresent();                   // each presented frame (starts the watchdog on the first)
void SetImageBase(uint64_t base);   // the guest image, for eboot+offset frames
} // namespace iss::freeze_watchdog
