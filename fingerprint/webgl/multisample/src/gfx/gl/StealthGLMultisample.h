/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_GL_STEALTH_GL_MULTISAMPLE_H_
#define GFX_GL_STEALTH_GL_MULTISAMPLE_H_

#include "GLContext.h"

namespace mozilla::gl {

// Whether a context keeps multisampling that Mozilla's driver workarounds in
// GLContext::InitImpl take away. aWasSupported is the feature as the driver
// reports it, before the workarounds ran.
bool StealthKeepsMultisample(bool aWasSupported, GLRenderer aRenderer);

}  // namespace mozilla::gl

#endif  // GFX_GL_STEALTH_GL_MULTISAMPLE_H_
