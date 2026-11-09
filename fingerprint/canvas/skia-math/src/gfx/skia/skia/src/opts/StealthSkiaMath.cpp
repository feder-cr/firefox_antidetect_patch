/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The state and the tables of StealthSkiaMath.h. Compiled once, for the baseline
// instruction set: no function of it runs in a raster pipeline stage.
//
// The mode (off, Intel, AMD) is read ONCE per process, from zoom.stealth.cpu.vendor, when
// the engine is on (zoom.stealth.fpp.hw_seed > 0), and kept in gMode; a pipeline stage
// never reads a pref. With the engine on and the pref missing or not "intel" or "amd" the
// process refuses, as the other declarations do (StealthRefuse); the gate in
// StealthDeclarationGate.cpp refuses the same at launch in the parent process. While the
// engine is off nothing is remembered: a launched browser gets its prefs after the process
// is up, and a stage that ran before them must not decide for the rest of the process.

#include "src/opts/StealthSkiaMath.h"

#ifndef STEALTH_SKIA_MATH_STANDALONE
#  include "mozilla/StaticPrefs_zoom.h"
#  include "mozilla/gfx/StealthRefuse.h"
#  include "nsString.h"
#endif

namespace StealthSkiaMath {

std::atomic<int> gMode{-1};

#include "src/opts/StealthSkiaMathTables.inc"

void SetModeForTest(int aMode) { gMode.store(aMode, std::memory_order_relaxed); }
void ReloadForTest() { gMode.store(-1, std::memory_order_relaxed); }

#ifdef STEALTH_SKIA_MATH_STANDALONE

// Outside Firefox (the proof that builds Skia alone) there are no prefs: the mode is the
// one SetModeForTest gave, and an unresolved mode is off.
int ResolveMode() { return kOff; }

#else

int ResolveMode() {
  // Not remembered while the engine is off.
  if (mozilla::StaticPrefs::zoom_stealth_fpp_hw_seed() <= 0) {
    return kOff;
  }
  int mode = kOff;
  {
    auto lock = mozilla::StaticPrefs::zoom_stealth_cpu_vendor();
    if (lock->EqualsLiteral("intel")) {
      mode = kIntel;
    } else if (lock->EqualsLiteral("amd")) {
      mode = kAmd;
    }
  }
  if (mode == kOff) {
    mozilla::gfx::StealthRefuse(
        "stealth: zoom.stealth.cpu.vendor is not declared as intel or amd. The "
        "estimate Skia's canvas starts its reciprocals from is the instruction of the "
        "host unless it is declared, and a missing declaration refuses rather than "
        "asking the host.");
  }
  if (!kRcpByMode[mode] || !kRsqByMode[mode]) {
    mozilla::gfx::StealthRefuse(
        "stealth: zoom.stealth.cpu.vendor is %s, whose RCPPS/RSQRTPS tables are not in "
        "this build (gen_stealth_skia_math.py writes them from the dump of such a CPU); "
        "a missing table refuses rather than borrowing another vendor's.",
        mode == kAmd ? "amd" : "intel");
  }
  int expected = -1;
  gMode.compare_exchange_strong(expected, mode, std::memory_order_relaxed);
  return gMode.load(std::memory_order_relaxed);
}

#endif  // STEALTH_SKIA_MATH_STANDALONE

}  // namespace StealthSkiaMath
