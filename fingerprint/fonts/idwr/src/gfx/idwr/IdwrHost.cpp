/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// What idwr asks of its host when built into the engine (IDWR_STATIC, see
// src/truetype/fatal.h): the one way a fatal error leaves the library, so that
// the crash report names the site instead of an anonymous abort().

#include "mozilla/Assertions.h"

extern "C" void idwr_host_fatal(const char* aSite) {
  MOZ_CRASH_UNSAFE_PRINTF("idwr: fatal error at %s", aSite);
}
