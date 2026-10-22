// SPDX-License-Identifier: GPL-2.0-or-later
// Tables dropped whole are destroyed on a thread of their own. Dropping the learned material
// templates at their cap (131072, each with its vectors and shared pointers) took the command
// processor 4 s of frees while the game waited for it (freeze-1.txt, RTX 4060, 2026-10-22).
#pragma once
#include <thread>
#include <utility>

namespace iss {
// Leaves container empty at once; its old contents go away on a detached thread.
template <class T>
void DestroyInBackground(T& container) {
    std::thread([old = std::move(container)]() mutable { T().swap(old); }).detach();
    container = T();
}
} // namespace iss
