/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_GL_STEALTH_GL_MULTISAMPLE_H_
#define GFX_GL_STEALTH_GL_MULTISAMPLE_H_

namespace mozilla::gl {

// Whether a context multisamples, decided by the declaration, not by the
// host's renderer. aDriverSupports is the feature as the driver reports it,
// before Mozilla's driver workarounds in GLContext::InitImpl ran.
bool StealthDeclaredMultisample(bool aDriverSupports);

}  // namespace mozilla::gl

#endif  // GFX_GL_STEALTH_GL_MULTISAMPLE_H_
