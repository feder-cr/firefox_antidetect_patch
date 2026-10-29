/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_STEALTHMEDIAENUMERATION_H_
#define DOM_MEDIA_STEALTHMEDIAENUMERATION_H_

#include <cstdint>
#include <utility>

#include "mozilla/StaticPrefs_zoom.h"
#include "nsThreadUtils.h"

namespace mozilla {

// Stealthfox [B300]: with the enumeration declared, the speakers are not
// asked of the host, for the list or for correlating a camera's groupId. They
// were the only part of an enumeration with fake cameras and microphones that
// went to the machine, through its audio stack, and its time was the host's:
// measured on Windows 250-350 ms on the first enumeration, on a host without
// audio 15-40.
inline bool StealthHostSpeakersMayBeAsked() {
  return StaticPrefs::zoom_stealth_media_enumeration_ms() == 0;
}

// Stealthfox [B300]: whether the host's speakers are enumerated. The one place
// that decides it, for the three that asked the flag directly. Takes the flag
// as a bool so this file never names MediaManager's private EnumerationFlag:
// the caller evaluates it inside MediaManager.
inline bool StealthHostSpeakersRequested(bool aEnumerateAudioOutputs) {
  return aEnumerateAudioOutputs && StealthHostSpeakersMayBeAsked();
}

// Stealthfox [B300]: a declared enumeration takes the declared time, on every
// host, instead of the host's speakers' time. The list is then kept as Firefox
// keeps it, so only the first one of the session waits, as on a real machine.
// With the pref at 0 aResolve runs now, on the same stack. aResolve is the
// resolve step of MediaManager::GetPhysicalDevices, passed as an rvalue; it is
// moved into the delayed runnable. Called on the main thread.
template <typename Resolve>
void StealthDeclaredEnumeration(Resolve&& aResolve) {
  if (uint32_t ms = StaticPrefs::zoom_stealth_media_enumeration_ms()) {
    NS_DelayedDispatchToCurrentThread(
        NS_NewRunnableFunction("MediaManager::DeclaredEnumeration",
                               std::forward<Resolve>(aResolve)),
        ms);
    return;
  }
  aResolve();
}

}  // namespace mozilla

#endif  // DOM_MEDIA_STEALTHMEDIAENUMERATION_H_
