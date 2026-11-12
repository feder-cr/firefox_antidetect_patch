/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef StealthParentalControls_h_
#define StealthParentalControls_h_

/**
 * Parental controls are declared, never read from the OS.
 *
 * Every implementation of nsIParentalControlsService reads the host in its
 * constructor (the Windows registry, the macOS preferences). With "on" Firefox
 * skips DoH for every name (TRR_PARENTAL_CONTROL), adds "Prefer: safe", blocks
 * downloads and restricts content; a host setting must change none of it. The
 * value is the one of a retail Firefox on a machine with no parental controls.
 *
 * The constructor calls this first and returns when it says so, before its
 * first OS call. Every reader (nsHttpHandler, the DoH heuristics, downloads,
 * CanonicalBrowsingContext) goes through the service and sees the same value.
 */
inline bool StealthParentalControlsDeclared(bool* aEnabled) {
  *aEnabled = false;
  return true;
}

#endif  // StealthParentalControls_h_
