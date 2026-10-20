// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace iss::native {
// ISS_RESOLUTION / ISS_HUD_ASPECT / ISS_FOV (ultrawide.cpp). Before guest entry; <0 on mismatch.
int InstallUltrawide(unsigned char* image, std::uint64_t image_size);
} // namespace iss::native
