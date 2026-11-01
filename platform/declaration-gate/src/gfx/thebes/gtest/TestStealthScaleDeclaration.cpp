/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// One scale for the window geometry (gfx::StealthScaleMatchesDeclaration). Run
// 41325657085 forced layout.css.devPixelsPerPx=1.5 over a 100% persona and the
// page read innerWidth 1920 inside an outerWidth of 1291; forced 1.0 over a
// 150% persona read outerWidth 1942 on a 1280 screen.

#include "StealthDeclarationGate.h"
#include "gtest/gtest.h"

using mozilla::gfx::StealthScaleMatchesDeclaration;

TEST(StealthScaleDeclaration, TheScaleTheDeclarationWasMadeAt)
{
  EXPECT_TRUE(StealthScaleMatchesDeclaration(1.0f, 1920, 1920));
  EXPECT_TRUE(StealthScaleMatchesDeclaration(1.25f, 1536, 1920));
  EXPECT_TRUE(StealthScaleMatchesDeclaration(1.5f, 1280, 1920));
  // A fractional CSS width, rounded by the core: 1707 x 1.5 = 2560.5.
  EXPECT_TRUE(StealthScaleMatchesDeclaration(1.5f, 1707, 2560));
  EXPECT_TRUE(StealthScaleMatchesDeclaration(1.75f, 1463, 2560));
}

TEST(StealthScaleDeclaration, AnyOtherScaleIsAnIdentityNoMachineHas)
{
  // The two cases of run 41325657085.
  EXPECT_FALSE(StealthScaleMatchesDeclaration(1.5f, 1920, 1920));
  EXPECT_FALSE(StealthScaleMatchesDeclaration(1.0f, 1280, 1920));
  // The host's own scale (-1), and nonsense.
  EXPECT_FALSE(StealthScaleMatchesDeclaration(-1.0f, 1920, 1920));
  EXPECT_FALSE(StealthScaleMatchesDeclaration(0.0f, 1920, 1920));
  EXPECT_FALSE(StealthScaleMatchesDeclaration(1.0f, 0, 1920));
  EXPECT_FALSE(StealthScaleMatchesDeclaration(1.0f, 1920, 0));
}
