/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_StealthParentalControls_h
#define mozilla_net_StealthParentalControls_h

namespace mozilla::net {

/**
 * Whether the resolver (and the "Prefer: safe" header) treats the session as
 * under parental controls. Declared, never read from the OS: Firefox asks the
 * OS's parental controls service for it (nsHttpHandler::
 * UpdateParentalControlsEnabled), and with the answer "on" TRR is skipped for
 * every name and the lookup goes to the OS resolver. A Windows machine with
 * parental controls on must not change what a page, or the network, sees; the
 * value is the one of a retail Firefox on a Windows with none.
 */
bool StealthParentalControlsEnabled();

}  // namespace mozilla::net

#endif  // mozilla_net_StealthParentalControls_h
