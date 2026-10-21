/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_STEALTHREFUSE_H_
#define MOZILLA_GFX_STEALTHREFUSE_H_

#include <stdarg.h>

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Sprintf.h"
#include "mozilla/glue/Debug.h"

namespace mozilla {
namespace gfx {

// The one way the stealth engine refuses to run: a declaration missing or
// malformed, a broken installation. The sentence goes to stderr FIRST and then
// into the crash. MOZ_CRASH alone keeps its text in the crash reason, and a
// release build prints nothing, so a launcher saw "the browser exited during
// startup ... printed: (nothing)" and every refusal read as a broken binary
// (firefox-51, 70-known-bugs.md [B367]). Every refusal goes through here, so
// none of them can be silent again.
[[noreturn]] MOZ_FORMAT_PRINTF(1, 2) inline void StealthRefuse(
    const char* aFormat, ...) {
  char message[1024];
  va_list args;
  va_start(args, aFormat);
  VsprintfLiteral(message, aFormat, args);
  va_end(args);
  printf_stderr("%s\n", message);
  MOZ_CRASH_UNSAFE_PRINTF("%s", message);
}

}  // namespace gfx
}  // namespace mozilla

#endif  // MOZILLA_GFX_STEALTHREFUSE_H_
